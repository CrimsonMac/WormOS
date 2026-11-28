/*
 * bh's side of a blackhole's input and output (see box.c).
 *
 * relay_terminal() takes the master of the pseudo-terminal init opened
 * inside the blackhole for a session, puts the host terminal in raw mode
 * and copies keys in and output out until the session is gone. Window
 * size changes are passed on. relay_output() copies output pipes to a
 * terminal.
 *
 * Output is filtered on the way: of the operating system commands (OSC),
 * only harmless ones pass (titles, colours, prompt marks; see
 * osc_allowed): OSC 52 could set (and on some terminals read) the host's
 * clipboard, OSC 777 show desktop notifications. An OSC starts with ESC ]
 * or with the 8-bit control OSC, raw (0x9d) or as UTF-8 (C2 9D), which
 * some terminals also act on. Everything else is passed through unchanged. When the session is gone, input the terminal
 * sent meanwhile (answers to queries a program inside made) is
 * discarded, so it cannot reach the host shell as typed commands.
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

/* Longest OSC sequence that is dropped before giving up on it. */
#define DROP_MAX (1u << 20)

enum { F_TEXT, F_ESC, F_C2, F_OSC_NUM, F_DROP, F_DROP_ESC };

struct filter {
	int state;
	char num[8];
	unsigned len;
	unsigned long dropped;
	char intro[2];          /* how the OSC began: ESC ], 0x9d or C2 9D */
	unsigned intro_len;
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
	/* Once the reader is gone, output is drained and dropped. */
	static int gone[3];

	if (fd >= 0 && fd < 3 && gone[fd])
		return 0;
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
			if (errno == EPIPE && fd >= 0 && fd < 3)
				gone[fd] = 1;
			return -1;
		}
		p += w;
		n -= w;
	}
	return 0;
}

/*
 * Operating system commands a program may send the host terminal: titles,
 * colours, the working directory and shell prompt marks. Everything else
 * is dropped: OSC 52 (clipboard), 8 (links, which open on the host when
 * clicked), 9, 99 and 777 (desktop notifications, easy to spoof), 1337,
 * 5113 and other terminal-specific extensions.
 */
static int osc_allowed(const char *num, unsigned len)
{
	static const char *const allowed[] = {
		"0", "1", "2", "4", "7", "10", "11", "12", "104", "110", "111", "112", "133",
	};

	for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
		if (strlen(allowed[i]) == len && !memcmp(allowed[i], num, len))
			return 1;
	return 0;
}

/* Copy data to fd, without the OSC sequences osc_allowed() refuses. */
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
			if (c == 0x1b) {
				f->state = F_ESC;
			} else if (c == 0xc2) {
				f->state = F_C2;
			} else if (c == 0x9d) {
				f->state = F_OSC_NUM;
				f->len = 0;
				f->intro[0] = (char)0x9d;
				f->intro_len = 1;
			} else {
				out[o++] = c;
			}
			break;
		case F_ESC:
			if (c == ']') {
				f->state = F_OSC_NUM;
				f->len = 0;
				f->intro[0] = 0x1b;
				f->intro[1] = ']';
				f->intro_len = 2;
				break;
			}
			/* ESC then an introducer or another ESC: terminals drop the
			 * first ESC, and so does this, so it can never join text that
			 * follows a dropped sequence into a new one (ESC + "]52;..."). */
			if (c == 0x1b)
				break;
			if (c == 0xc2 || c == 0x9d) {
				f->state = F_TEXT;
				goto again;
			}
			out[o++] = 0x1b;
			out[o++] = c;
			f->state = F_TEXT;
			break;
		case F_C2:
			if (c == 0x9d) {
				f->state = F_OSC_NUM;
				f->len = 0;
				f->intro[0] = (char)0xc2;
				f->intro[1] = (char)0x9d;
				f->intro_len = 2;
				break;
			}
			out[o++] = (char)0xc2;
			f->state = F_TEXT;
			goto again;
		case F_OSC_NUM:
			if (c >= '0' && c <= '9' && f->len < sizeof(f->num)) {
				f->num[f->len++] = c;
				break;
			}
			/* Another escape right after the header: terminals abort
			 * the header, and so does this, so a sequence dropped next
			 * can never leave it open for the text after it ("ESC ]" +
			 * dropped + "52;..." would read as OSC 52). */
			if (c == 0x1b || c == 0xc2 || c == 0x9d) {
				f->state = F_TEXT;
				goto again;
			}
			/* No number: not an OSC terminals act on (the console's
			 * palette sequence has no terminator to drop up to). */
			if (f->len == 0 || osc_allowed(f->num, f->len)) {
				memcpy(out + o, f->intro, f->intro_len);
				o += f->intro_len;
				memcpy(out + o, f->num, f->len);
				o += f->len;
				f->state = F_TEXT;
				goto again;
			}
			/* Anything else goes, up to its terminator (BEL or ST). */
			f->dropped = 0;
			f->state = c == 0x07 ? F_TEXT : c == 0x1b ? F_DROP_ESC : F_DROP;
			break;
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

void relay_terminal(int master)
{
	struct sigaction sa = { .sa_handler = on_winch };
	struct termios saved, raw;
	struct filter f = { 0 };
	char in[4096], buf[8192];
	size_t in_len = 0, in_off = 0;
	int input_open = 1;

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
