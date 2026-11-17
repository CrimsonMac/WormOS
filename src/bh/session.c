/*
 * bh's side of a session: ask the blackhole's init for a program, relay
 * its terminal or output, and return its exit code (see box.c).
 *
 * In the terminal's foreground, the program gets a pseudo-terminal of the
 * blackhole's own with the host terminal's modes and size; bh relays keys
 * and output (relay.c). Otherwise (pipes, or a background job, which may
 * not set raw mode or flush without SIGTTOU) the program gets bh's own
 * input and output, except that input from a terminal becomes /dev/null
 * and output to a terminal goes through pipes that bh relays and filters.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "bh.h"

ssize_t session_recv(int sock, void *buf, size_t len, int *fds, int nfds)
{
	char ctl[CMSG_SPACE(3 * sizeof(int))];
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
			      .msg_control = ctl, .msg_controllen = sizeof(ctl) };
	ssize_t r;

	for (int i = 0; i < nfds; i++)
		fds[i] = -1;
	while ((r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR)
		;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); r >= 0 && cm; cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
			continue;
		int n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		int got[3];

		memcpy(got, CMSG_DATA(cm), (n > 3 ? 3 : n) * sizeof(int));
		for (int i = 0; i < n && i < 3; i++) {
			if (i < nfds)
				fds[i] = got[i];
			else
				close(got[i]);
		}
	}
	if (msg.msg_flags & MSG_CTRUNC) {
		for (int i = 0; i < nfds; i++)
			if (fds[i] >= 0)
				close(fds[i]);
		errno = EMSGSIZE;
		return -1;
	}
	return r;
}

int session_send(int sock, const void *buf, size_t len, const int *fds, int nfds)
{
	char ctl[CMSG_SPACE(3 * sizeof(int))] = { 0 };
	struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t r;

	if (nfds > 0) {
		struct cmsghdr *cm;

		msg.msg_control = ctl;
		msg.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
		cm = CMSG_FIRSTHDR(&msg);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(nfds * sizeof(int));
		memcpy(CMSG_DATA(cm), fds, nfds * sizeof(int));
	}
	while ((r = sendmsg(sock, &msg, MSG_NOSIGNAL)) < 0 && errno == EINTR)
		;
	return r == (ssize_t)len ? 0 : -1;
}

/* Wait for the exit code; -1 if the blackhole went away first. */
static int wait_exit(int conn)
{
	char msg[512];
	ssize_t r;
	int code;

	for (;;) {
		r = session_recv(conn, msg, sizeof(msg) - 1, NULL, 0);
		if (r <= 0)
			return -1;
		if (msg[0] == SESSION_EXIT && r == 1 + (ssize_t)sizeof(int)) {
			memcpy(&code, msg + 1, sizeof(int));
			return code;
		}
		if (msg[0] == SESSION_ERROR) {
			msg[r] = '\0';
			fprintf(stderr, "bh: %s\n", msg + 1);
			return 1;
		}
	}
}

int session_run(int conn, char *const argv[], const char *name)
{
	static char buf[SESSION_MAX];
	struct request *req = (struct request *)buf;
	int out[2] = { -1, -1 }, err[2] = { -1, -1 }, fds[3], null = -1, code;
	const char *term = getenv("TERM");
	size_t len = sizeof(*req);

	(void)name;
	memset(req, 0, sizeof(*req));
	req->magic = SESSION_MAGIC;
	snprintf(req->term, sizeof(req->term), "%s", term ? term : "linux");
	for (int i = 0; argv && argv[i]; i++) {
		size_t n = strlen(argv[i]) + 1;

		if (len + n > sizeof(buf) || i >= 255)
			die("the command is too long");
		memcpy(buf + len, argv[i], n);
		len += n;
		req->argc++;
	}

	/* Only in the terminal's foreground: a background job cannot set raw mode. */
	req->pty = isatty(0) && tcgetpgrp(0) == getpgrp() && tcgetattr(0, &req->termios) == 0;
	if (req->pty) {
		ioctl(0, TIOCGWINSZ, &req->size);
		/* Output to a file or pipe keeps plain newlines. */
		if (!isatty(1))
			req->termios.c_oflag &= ~OPOST;
		if (session_send(conn, buf, len, NULL, 0) < 0)
			die("cannot reach the blackhole: %s", strerror(errno));
	} else {
		fds[0] = 0;
		if (isatty(0)) {
			if ((null = open("/dev/null", O_RDONLY | O_CLOEXEC)) < 0)
				die("cannot open /dev/null: %s", strerror(errno));
			fds[0] = null;
		}
		fds[1] = 1;
		fds[2] = 2;
		if (isatty(1)) {
			if (pipe2(out, O_CLOEXEC) < 0)
				die("cannot create a pipe: %s", strerror(errno));
			fds[1] = out[1];
		}
		if (isatty(2)) {
			if (pipe2(err, O_CLOEXEC) < 0)
				die("cannot create a pipe: %s", strerror(errno));
			fds[2] = err[1];
		}
		if (session_send(conn, buf, len, fds, 3) < 0)
			die("cannot reach the blackhole: %s", strerror(errno));
		if (null >= 0)
			close(null);
		if (out[1] >= 0)
			close(out[1]);
		if (err[1] >= 0)
			close(err[1]);
	}

	if (req->pty) {
		char msg[512];
		int master;
		ssize_t r = session_recv(conn, msg, sizeof(msg) - 1, &master, 1);

		if (r <= 0)
			return -1;      /* init failed and said why */
		if (msg[0] == SESSION_ERROR) {
			msg[r] = '\0';
			fprintf(stderr, "bh: %s\n", msg + 1);
			return 1;
		}
		if (msg[0] != SESSION_MASTER || master < 0)
			return -1;
		relay_terminal(master);
	} else {
		relay_output(out[0], err[0]);
	}
	code = wait_exit(conn);
	return code;
}
