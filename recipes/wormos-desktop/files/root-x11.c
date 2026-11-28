/*
 * root-x11 - an X server of root's own for the root terminal.
 *
 * X11 has no isolation between the clients of one server: any of them can
 * read the keys typed into another's windows. So root's X11 programs (Java
 * tools such as Ghidra, run from the root terminal) must not share the
 * desktop's Xwayland, where every program of the user could watch them.
 *
 * root-x11 SHELL-PID, run as root by root-shell, takes a free display
 * number N, listens on /tmp/.X11-unix/XN (root's alone: 0700, no abstract
 * socket, which would have no permissions), prints N and goes to the
 * background. When the first client connects, it starts a nested labwc as
 * a window on the desktop (its configuration in /usr/share/worm/root-x11),
 * whose Xwayland, like every Xwayland, admits only its own user (root),
 * and relays each connection to that Xwayland, file descriptors included
 * (MIT-SHM, Present). The window closes a minute after the last root X11
 * program; everything ends with the root shell.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAIRS   64
#define MAX_FDS     16
#define IDLE_SECS   60
#define RUNTIME     "/run/user/0"
#define CONFIG      "/usr/share/worm/root-x11"

struct pair {
	int fd[2];      /* client, server */
};

static struct pair pairs[MAX_PAIRS];
static int n_pairs, display = -1, listen_fd = -1;
static pid_t nested;
static char display_file[64], lock_path[64], sock_path[64];

static void cleanup(void)
{
	if (nested > 0)
		kill(nested, SIGTERM);
	if (sock_path[0])
		unlink(sock_path);
	if (lock_path[0])
		unlink(lock_path);
	if (display_file[0])
		unlink(display_file);
}

static void on_term(int sig)
{
	(void)sig;
	cleanup();
	_exit(0);
}

/* A display number nobody uses, with the X lock file made for it. */
static int take_display(void)
{
	for (int n = 20; n < 100; n++) {
		char text[16];
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		int fd;

		snprintf(lock_path, sizeof(lock_path), "/tmp/.X%d-lock", n);
		snprintf(sock_path, sizeof(sock_path), "/tmp/.X11-unix/X%d", n);
		if (access(sock_path, F_OK) == 0)
			continue;
		if ((fd = open(lock_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0444)) < 0)
			continue;
		snprintf(text, sizeof(text), "%10d\n", (int)getpid());
		if (write(fd, text, strlen(text)) < 0) {
			/* The lock is the file itself. */
		}
		close(fd);
		if ((listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
			break;
		snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
		umask(077);
		if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
		    chmod(sock_path, 0700) < 0 || listen(listen_fd, 16) < 0) {
			close(listen_fd);
			unlink(lock_path);
			lock_path[0] = '\0';
			continue;
		}
		return n;
	}
	sock_path[0] = lock_path[0] = '\0';
	return -1;
}

/* Start the nested labwc; its Xwayland's display number, or -1. */
static int start_nested(void)
{
	char buf[32] = "";
	int fd, n;

	unlink(display_file);
	nested = fork();
	if (nested < 0)
		return -1;
	if (nested == 0) {
		setsid();
		setenv("XDG_RUNTIME_DIR", RUNTIME, 1);
		setenv("ROOT_X11_DISPLAY_FILE", display_file, 1);
		setenv("WLR_BACKENDS", "wayland", 1);
		setenv("WLR_RENDERER", "pixman", 1);
		unsetenv("DISPLAY");
		if (access("/usr/lib/xwayland/xwayland-gles", X_OK) == 0)
			setenv("WLR_XWAYLAND", "/usr/lib/xwayland/xwayland-gles", 1);
		if ((fd = open("/dev/null", O_RDWR | O_CLOEXEC)) >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execl("/usr/bin/labwc", "labwc", "-C", CONFIG, (char *)NULL);
		_exit(127);
	}
	/* Its autostart writes the display it got. */
	for (int i = 0; i < 200; i++) {
		if ((fd = open(display_file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) >= 0) {
			n = (int)read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (n > 1 && buf[n - 1] == '\n' && buf[0] == ':')
				return atoi(buf + 1);
		}
		if (waitpid(nested, NULL, WNOHANG) == nested) {
			nested = 0;
			return -1;
		}
		usleep(50000);
	}
	kill(nested, SIGTERM);
	return -1;
}

static int connect_display(int n)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/.X11-unix/X%d", n);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	if (fd >= 0)
		fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

/* Copy what one side sent to the other, with the descriptors it carries. */
static int relay(int from, int to)
{
	char data[65536];
	union {
		char buf[CMSG_SPACE(MAX_FDS * sizeof(int))];
		struct cmsghdr align;
	} ctl;
	struct iovec iov = { data, sizeof(data) };
	struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf,
			    .msg_controllen = sizeof(ctl.buf) };
	struct cmsghdr *c;
	int fds[MAX_FDS], nfds = 0, ok;
	ssize_t n = recvmsg(from, &m, MSG_CMSG_CLOEXEC);
	size_t off = 0;

	if (n <= 0)
		return n < 0 && (errno == EINTR || errno == EAGAIN) ? 0 : -1;
	for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));

			for (int i = 0; i < k && nfds < MAX_FDS; i++)
				memcpy(&fds[nfds++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
		}
	ok = !(m.msg_flags & MSG_CTRUNC);
	/* The descriptors go with the first part of the data, as they came. */
	while (ok && off < (size_t)n) {
		struct iovec out = { data + off, (size_t)n - off };
		struct msghdr s = { .msg_iov = &out, .msg_iovlen = 1 };
		ssize_t w;

		if (nfds && off == 0) {
			s.msg_control = ctl.buf;
			s.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
			c = CMSG_FIRSTHDR(&s);
			c->cmsg_level = SOL_SOCKET;
			c->cmsg_type = SCM_RIGHTS;
			c->cmsg_len = CMSG_LEN(nfds * sizeof(int));
			memcpy(CMSG_DATA(c), fds, nfds * sizeof(int));
		}
		w = sendmsg(to, &s, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd p = { .fd = to, .events = POLLOUT };

				poll(&p, 1, 1000);
				continue;
			}
			ok = 0;
			break;
		}
		off += (size_t)w;
	}
	for (int i = 0; i < nfds; i++)
		close(fds[i]);
	return ok ? 0 : -1;
}

static void drop_pair(int i)
{
	close(pairs[i].fd[0]);
	close(pairs[i].fd[1]);
	pairs[i] = pairs[--n_pairs];
}

int main(int argc, char **argv)
{
	pid_t shell;
	int shell_fd, xdisplay = -1;
	time_t idle_since = time(NULL);

	if (argc != 2 || geteuid() != 0 || (shell = atoi(argv[1])) <= 1) {
		fputs("usage: root-x11 SHELL-PID (as root)\n", stderr);
		return 2;
	}
	if ((shell_fd = (int)syscall(SYS_pidfd_open, shell, 0)) < 0) {
		perror("root-x11: pidfd_open");
		return 1;
	}
	mkdir(RUNTIME, 0700);
	if ((display = take_display()) < 0) {
		fputs("root-x11: no free display\n", stderr);
		return 1;
	}
	snprintf(display_file, sizeof(display_file), RUNTIME "/root-x11-%d", display);
	printf("%d\n", display);
	fflush(stdout);
	switch (fork()) {
	case -1:
		cleanup();
		return 1;
	case 0:
		break;
	default:
		_exit(0);
	}
	setsid();
	{
		int fd = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
		}
	}
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGHUP, on_term);
	signal(SIGPIPE, SIG_IGN);

	for (;;) {
		struct pollfd p[2 + 2 * MAX_PAIRS];
		int np = 0;

		p[np++] = (struct pollfd){ .fd = shell_fd, .events = POLLIN };
		p[np++] = (struct pollfd){ .fd = n_pairs < MAX_PAIRS ? listen_fd : -1, .events = POLLIN };
		for (int i = 0; i < n_pairs; i++) {
			p[np++] = (struct pollfd){ .fd = pairs[i].fd[0], .events = POLLIN };
			p[np++] = (struct pollfd){ .fd = pairs[i].fd[1], .events = POLLIN };
		}
		if (poll(p, np, 5000) < 0 && errno != EINTR)
			break;
		if (p[0].revents)
			break;  /* the root shell ended */
		if (nested > 0 && waitpid(nested, NULL, WNOHANG) == nested) {
			nested = 0;
			xdisplay = -1;
		}
		/* Relays first (from the end, as finished pairs are dropped). */
		for (int i = n_pairs - 1; i >= 0; i--) {
			short a = p[2 + 2 * i].revents, b = p[3 + 2 * i].revents;

			if ((a && relay(pairs[i].fd[0], pairs[i].fd[1]) < 0) ||
			    (b && relay(pairs[i].fd[1], pairs[i].fd[0]) < 0))
				drop_pair(i);
		}
		if (p[1].revents & POLLIN) {
			int client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK), server = -1;

			if (client >= 0) {
				if (xdisplay < 0 || (server = connect_display(xdisplay)) < 0) {
					if (nested > 0)
						kill(nested, SIGTERM);
					nested = 0;
					xdisplay = start_nested();
					if (xdisplay >= 0)
						server = connect_display(xdisplay);
				}
				if (server < 0) {
					close(client);
				} else {
					pairs[n_pairs].fd[0] = client;
					pairs[n_pairs].fd[1] = server;
					n_pairs++;
				}
			}
		}
		/* The window goes a while after the last root X11 program. */
		if (n_pairs)
			idle_since = time(NULL);
		else if (nested > 0 && time(NULL) - idle_since > IDLE_SECS) {
			kill(nested, SIGTERM);
			waitpid(nested, NULL, 0);
			nested = 0;
			xdisplay = -1;
		}
	}
	cleanup();
	return 0;
}
