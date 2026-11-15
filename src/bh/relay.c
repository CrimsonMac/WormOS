/*
 * bh's side of a blackhole's input and output (see box.c).
 *
 * relay_terminal() receives the master of the pseudo-terminal init opened
 * inside the blackhole, puts the host terminal in raw mode and copies keys
 * in and output out until the blackhole is gone. Window size changes are
 * passed on. relay_output() copies output pipes to a terminal.
 *
 * Output is filtered on the way: OSC 52 sequences, with which a program
 * could set (and on some terminals read) the host's clipboard, are
 * dropped. Everything else is passed through unchanged. When the
 * blackhole is gone, input the terminal sent meanwhile (answers to
 * queries a program inside made) is discarded, so it cannot reach the
 * host shell as typed commands.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "bh.h"

/* Longest OSC 52 sequence that is dropped before giving up on it. */
#define DROP_MAX (1u << 20)

enum { F_TEXT, F_ESC, F_OSC_NUM, F_DROP, F_DROP_ESC };

struct filter {
	int state;
	char num[8];
	unsigned len;
	unsigned long dropped;
};

static int wake[2] = { -1, -1 };
static volatile sig_atomic_t resized;

static void on_winch(int sig)
{
	int saved = errno;

	(void)sig;
	resized = 1;
	if (wake[1] >= 0 && write(wake[1], "w", 1) < 0) {
		/* The pipe is full: a wakeup is already pending. */
	}
	errno = saved;
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n > 0) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd pfd = { .fd = fd, .events = POLLOUT };

				poll(&pfd, 1, -1);
				continue;
			}
			return -1;
		}
		p += w;
		n -= w;
	}
	return 0;
}

/* Copy data to fd, without OSC 52 sequences. */
static void filter_write(int fd, struct filter *f, const char *data, size_t n)
{
	char out[8192 + 16];
	size_t o = 0;

	for (size_t i = 0; i < n; i++) {
		unsigned char c = data[i];

		if (o > sizeof(out) - 16) {
			write_all(fd, out, o);
			o = 0;
		}
again:
		switch (f->state) {
		case F_TEXT:
			if (c == 0x1b)
				f->state = F_ESC;
			else
				out[o++] = c;
			break;
		case F_ESC:
			if (c == ']') {
				f->state = F_OSC_NUM;
				f->len = 0;
				break;
			}
			out[o++] = 0x1b;
			if (c != 0x1b) {
				out[o++] = c;
				f->state = F_TEXT;
			}
			break;
		case F_OSC_NUM:
			if (c >= '0' && c <= '9' && f->len < sizeof(f->num)) {
				f->num[f->len++] = c;
				break;
			}
			if (c == ';' && f->len == 2 && !memcmp(f->num, "52", 2)) {
				f->state = F_DROP;
				f->dropped = 0;
				break;
			}
			out[o++] = 0x1b;
			out[o++] = ']';
			memcpy(out + o, f->num, f->len);
			o += f->len;
			f->state = F_TEXT;
			goto again;
		case F_DROP:
		case F_DROP_ESC:
			if (c == 0x07 || (f->state == F_DROP_ESC && c == '\\') ||
			    ++f->dropped > DROP_MAX)
				f->state = F_TEXT;
			else
				f->state = c == 0x1b ? F_DROP_ESC : F_DROP;
			break;
		}
	}
	if (o)
		write_all(fd, out, o);
}

static int receive_fd(int sock)
{
	char ctl[CMSG_SPACE(sizeof(int))], byte;
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
			      .msg_control = ctl, .msg_controllen = sizeof(ctl) };
	struct cmsghdr *cm;
	int fd = -1;
	ssize_t r;

	while ((r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR)
		;
	if (r != 1)
		return -1;
	for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
			memcpy(&fd, CMSG_DATA(cm), sizeof(int));
	return fd;
}

void relay_terminal(int sock, pid_t child)
{
	struct sigaction sa = { .sa_handler = on_winch };
	struct termios saved, raw;
	struct filter f = { 0 };
	char in[4096], buf[8192];
	size_t in_len = 0, in_off = 0;
	int master, input_open = 1;

	(void)child;
	master = receive_fd(sock);
	close(sock);
	if (master < 0)
		return;         /* init failed and said why */
	fcntl(master, F_SETFL, O_NONBLOCK);
	if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) < 0)
		wake[0] = wake[1] = -1;
	sigaction(SIGWINCH, &sa, NULL);

	tcgetattr(0, &saved);
	raw = saved;
	cfmakeraw(&raw);
	tcsetattr(0, TCSANOW, &raw);
	/* The size may have changed while the blackhole started. */
	resized = 1;

	for (;;) {
		struct pollfd p[3] = {
			{ .fd = master, .events = POLLIN | (in_len ? POLLOUT : 0) },
			{ .fd = input_open && !in_len ? 0 : -1, .events = POLLIN },
			{ .fd = wake[0], .events = POLLIN },
		};
		ssize_t r;

		if (resized) {
			struct winsize ws;

			resized = 0;
			if (ioctl(0, TIOCGWINSZ, &ws) == 0)
				ioctl(master, TIOCSWINSZ, &ws);
		}
		if (poll(p, 3, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (p[2].revents & POLLIN)
			while (read(wake[0], buf, sizeof(buf)) > 0)
				;
		if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			r = read(master, buf, sizeof(buf));
			if (r > 0)
				filter_write(1, &f, buf, r);
			else if (r == 0 || (errno != EAGAIN && errno != EINTR))
				break;  /* EIO: nothing inside has the terminal open */
		}
		if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			r = read(0, in, sizeof(in));
			if (r > 0) {
				in_len = r;
				in_off = 0;
			} else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
				input_open = 0;
			}
		}
		if (in_len && (p[0].revents & POLLOUT || p[1].revents)) {
			r = write(master, in + in_off, in_len);
			if (r > 0) {
				in_off += r;
				in_len -= r;
			} else if (r < 0 && errno != EAGAIN && errno != EINTR) {
				in_len = 0;
			}
		}
	}
	close(master);
	signal(SIGWINCH, SIG_DFL);
	/* Answers the terminal sent to programs that are gone go nowhere. */
	tcsetattr(0, TCSADRAIN, &saved);
	usleep(50000);
	tcflush(0, TCIFLUSH);
}

void relay_output(int out, int err)
{
	struct filter f[2] = { 0 };
	int fds[2] = { out, err };
	char buf[8192];

	while (fds[0] >= 0 || fds[1] >= 0) {
		struct pollfd p[2] = {
			{ .fd = fds[0], .events = POLLIN },
			{ .fd = fds[1], .events = POLLIN },
		};

		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (int i = 0; i < 2; i++) {
			ssize_t r;

			if (fds[i] < 0 || !(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			r = read(fds[i], buf, sizeof(buf));
			if (r > 0) {
				filter_write(i + 1, &f[i], buf, r);
			} else if (r == 0 || errno != EINTR) {
				close(fds[i]);
				fds[i] = -1;
			}
		}
	}
	/* Only the terminal's foreground may flush it (others get SIGTTOU). */
	if (isatty(0) && tcgetpgrp(0) == getpgrp())
		tcflush(0, TCIFLUSH);
}
