/*
 * wormlogin - the graphical login on the first console (worminit service).
 *
 * It runs the login screen: labwc with the greeter program, as the
 * unprivileged user "greeter". The greeter asks for a password and sends
 * it here over a socket (/run/wormlogin/socket, only the greeter may
 * connect); wormlogin checks it against /etc/shadow, ends the login
 * screen and starts the user's desktop (/usr/lib/worm/session) as that
 * user. When the desktop ends, the login screen comes back.
 *
 * While no user account exists, the greeter shows the first-boot setup
 * instead, and wormlogin creates the account with
 * "/usr/lib/worm/first-boot --create", then logs the new user in.
 *
 * Messages are SOCK_SEQPACKET datagrams of NUL-separated fields:
 *
 *     login USER PASSWORD
 *     setup NAME FULL-NAME PASSWORD ROOT-PASSWORD LAYOUT ZONE
 *     power off|reboot
 *
 * answered by "ok" or "fail" and a message.
 *
 * In the live system (/run/wormos/live, made by /usr/lib/worm/live-setup
 * when the ISO started), the user "live" is logged in at once, every time.
 *
 * If the login screen cannot run (no graphics device, no greeter user, or
 * it ends three times in a row without a login), the console login takes
 * over until the next restart (/usr/lib/worm/console tty1).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "password.h"

#define RUN_DIR        "/run/wormlogin"
#define SOCKET_PATH    RUN_DIR "/socket"
#define GREETER_HOME   RUN_DIR "/greeter"
#define FALLBACK       RUN_DIR "/console"
#define TTY            "/dev/tty1"
#define GREETER_USER   "greeter"
#define GREETER_CMD    "/usr/lib/worm/greeter-session"
#define SESSION_CMD    "exec /usr/lib/worm/session"
#define CONSOLE        "/usr/lib/worm/console"
#define FIRST_BOOT     "/usr/lib/worm/first-boot"
#define MAX_FAILURES   3

static char session_user[64];   /* set by a successful login or setup */

/* Logins and failures go to the system log (authpriv). */
static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(LOG_AUTHPRIV | LOG_NOTICE, fmt, ap);
	va_end(ap);
}

/* The console login, from now until the next restart. */
static void console(const char *why)
{
	int fd;

	say("%s; using the console login", why);
	if ((fd = open(FALLBACK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0)
		close(fd);
	execl(CONSOLE, CONSOLE, "tty1", (char *)NULL);
	say("cannot run %s: %s", CONSOLE, strerror(errno));
	sleep(5);
	exit(1);
}

static int users_exist(void)
{
	struct passwd *pw;
	int found = 0;

	setpwent();
	while ((pw = getpwent()))
		if (pw->pw_uid >= 1000 && pw->pw_uid < 65534)
			found = 1;
	endpwent();
	return found;
}

/* Make tty1 the controlling terminal and standard input. */
static void take_tty(void)
{
	int fd;

	setsid();
	if ((fd = open(TTY, O_RDWR | O_NOCTTY)) < 0)
		return;
	ioctl(fd, TIOCSCTTY, 1);
	dup2(fd, 0);
	if (fd > 2)
		close(fd);
}

static void log_to(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		close(fd);
	}
}

static gid_t group_id(const char *name)
{
	struct group *g = getgrnam(name);

	return g ? g->gr_gid : (gid_t)-1;
}

/* ---- the login screen ---- */

static pid_t start_greeter(struct passwd *greeter, int setup)
{
	pid_t pid = fork();

	if (pid != 0)
		return pid;
	take_tty();
	log_to(RUN_DIR "/greeter.log");
	{
		/* The devices it needs, without being listed in /etc/group. */
		gid_t groups[3];
		int n = 0;

		if ((groups[n] = group_id("video")) != (gid_t)-1)
			n++;
		if ((groups[n] = group_id("input")) != (gid_t)-1)
			n++;
		if ((groups[n] = group_id("render")) != (gid_t)-1)
			n++;
		if (setgroups(n, groups) < 0 || setgid(greeter->pw_gid) < 0 ||
		    setuid(greeter->pw_uid) < 0)
			_exit(127);
	}
	clearenv();
	setenv("HOME", GREETER_HOME, 1);
	setenv("USER", GREETER_USER, 1);
	setenv("LOGNAME", GREETER_USER, 1);
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	setenv("XDG_RUNTIME_DIR", GREETER_HOME, 1);
	setenv("WORMOS_GREETER_MODE", setup ? "setup" : "login", 1);
	if (chdir(GREETER_HOME) < 0)
		_exit(127);
	execl(GREETER_CMD, GREETER_CMD, (char *)NULL);
	_exit(127);
}

static int listen_socket(struct passwd *greeter)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    chown(SOCKET_PATH, greeter->pw_uid, greeter->pw_gid) < 0 ||
	    chmod(SOCKET_PATH, 0600) < 0 || listen(fd, 4) < 0) {
		say("cannot create %s: %s", SOCKET_PATH, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

static void reply(int fd, const char *status, const char *message)
{
	char buf[512];
	size_t n = snprintf(buf, sizeof(buf), "%s", status) + 1;

	if (message)
		n += snprintf(buf + n, sizeof(buf) - n, "%s", message);
	send(fd, buf, n, MSG_NOSIGNAL);
}

static int valid_login_user(const char *name)
{
	struct passwd *pw = getpwnam(name);

	return pw && pw->pw_uid >= 1000 && pw->pw_uid < 65534;
}

static int has_newline(const char *s)
{
	return strchr(s, '\n') || strchr(s, '\r');
}

/* first-boot --create; its last line of output goes into msg. */
static int create_account(char **f, char *msg, size_t len)
{
	char out[4096];
	int in[2], err[2], status;
	size_t used = 0;
	pid_t pid;
	ssize_t n;

	if (pipe2(in, O_CLOEXEC) < 0 || pipe2(err, O_CLOEXEC) < 0)
		return -1;
	pid = fork();
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(err[1], 2);
		dup2(err[1], 1);
		execl(FIRST_BOOT, FIRST_BOOT, "--create", f[1], f[2], f[5], f[6], (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(err[1]);
	dprintf(in[1], "%s\n%s\n", f[3], f[4]);
	close(in[1]);
	/* Read everything: closing the pipe early would kill the script. */
	while ((n = read(err[0], out + used, sizeof(out) - 1 - used)) > 0 ||
	       (n < 0 && errno == EINTR))
		if (n > 0 && (used += n) == sizeof(out) - 1)
			used = 0;
	out[used] = '\0';
	close(err[0]);
	while (used > 0 && out[used - 1] == '\n')
		out[--used] = '\0';
	{
		const char *last = strrchr(out, '\n') ? strrchr(out, '\n') + 1 : out;
		size_t l = strlen(last) < len ? strlen(last) : len - 1;

		memcpy(msg, last, l);
		msg[l] = '\0';
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void handle(int fd, char *buf, size_t len)
{
	char *f[8];
	int n = 0;

	/* Split the NUL-separated fields. */
	buf[len] = '\0';
	for (size_t i = 0; i <= len && n < 8; ) {
		f[n++] = buf + i;
		i += strlen(buf + i) + 1;
		if (i > len)
			break;
	}

	if (n == 3 && !strcmp(f[0], "login")) {
		if (!session_user[0] && valid_login_user(f[1]) && password_ok(f[1], f[2])) {
			snprintf(session_user, sizeof(session_user), "%s", f[1]);
			say("%s logged in", f[1]);
			reply(fd, "ok", NULL);
		} else {
			if (!valid_login_user(f[1]))
				sleep(2);
			say("failed login for %s", f[1]);
			reply(fd, "fail", "Wrong user name or password.");
		}
	} else if (n == 7 && !strcmp(f[0], "setup")) {
		char msg[256] = "";

		if (users_exist())
			reply(fd, "fail", "The setup is already done.");
		else if (has_newline(f[3]) || has_newline(f[4]) || !*f[3] || !*f[4])
			reply(fd, "fail", "The passwords cannot be empty or contain line breaks.");
		else if (create_account(f, msg, sizeof(msg)) < 0) {
			say("the setup failed: %s", msg);
			reply(fd, "fail", *msg ? msg : "Creating the account failed.");
		} else {
			snprintf(session_user, sizeof(session_user), "%s", f[1]);
			say("first-boot setup created %s", f[1]);
			reply(fd, "ok", NULL);
		}
	} else if (n == 2 && !strcmp(f[0], "power") &&
		   (!strcmp(f[1], "off") || !strcmp(f[1], "reboot"))) {
		reply(fd, "ok", NULL);
		sync();
		kill(1, !strcmp(f[1], "off") ? SIGUSR2 : SIGTERM);
	} else {
		reply(fd, "fail", "Unknown request.");
	}
	explicit_bzero(buf, len);
}

/* Run the login screen until it ends; returns its exit status (-1: killed). */
static int run_greeter(struct passwd *greeter, int setup)
{
	struct pollfd fds[2];
	int listener = listen_socket(greeter), status = -1, nfds = 1;
	time_t ended = 0;
	pid_t pid;

	if (listener < 0)
		return -1;
	session_user[0] = '\0';
	pid = start_greeter(greeter, setup);
	fds[0] = (struct pollfd){ .fd = listener, .events = POLLIN };
	for (;;) {
		pid_t done = waitpid(pid, &status, WNOHANG);

		if (done == pid)
			break;
		/* After a login the greeter ends itself; make sure it does. */
		if (session_user[0] && !ended)
			ended = time(NULL);
		if (ended && time(NULL) - ended > 10)
			kill(pid, SIGTERM);
		if (poll(fds, nfds, 250) <= 0)
			continue;
		if (fds[0].revents & POLLIN) {
			int c = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
			struct ucred cred;
			socklen_t clen = sizeof(cred);

			if (c >= 0 && (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 ||
				       cred.uid != greeter->pw_uid || nfds == 2)) {
				close(c);
				c = -1;
			}
			if (c >= 0) {
				fds[1] = (struct pollfd){ .fd = c, .events = POLLIN };
				nfds = 2;
			}
		}
		if (nfds == 2 && fds[1].revents) {
			char buf[2048];
			ssize_t len = recv(fds[1].fd, buf, sizeof(buf) - 1, 0);

			if (len <= 0) {
				close(fds[1].fd);
				nfds = 1;
			} else {
				handle(fds[1].fd, buf, len);
			}
		}
	}
	if (nfds == 2)
		close(fds[1].fd);
	close(listener);
	unlink(SOCKET_PATH);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ---- the user's desktop ---- */

static void run_session(const char *name)
{
	struct passwd *pw = getpwnam(name);
	char runtime[64];
	int status;
	pid_t pid;

	if (!pw)
		return;
	snprintf(runtime, sizeof(runtime), "/run/user/%u", (unsigned)pw->pw_uid);
	mkdir("/run/user", 0755);
	if (mkdir(runtime, 0700) == 0 || errno == EEXIST) {
		/* The folder itself, never what a link there points to. */
		int dfd = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

		if (dfd < 0 || fchown(dfd, pw->pw_uid, pw->pw_gid) < 0 || fchmod(dfd, 0700) < 0)
			say("cannot prepare %s", runtime);
		if (dfd >= 0)
			close(dfd);
	}

	pid = fork();
	if (pid == 0) {
		const char *shell = pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";

		take_tty();
		dup2(0, 1);
		dup2(0, 2);
		if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 ||
		    setuid(pw->pw_uid) < 0)
			_exit(127);
		if (chdir(pw->pw_dir) < 0 && chdir("/") < 0)
			_exit(127);
		clearenv();
		setenv("HOME", pw->pw_dir, 1);
		setenv("USER", pw->pw_name, 1);
		setenv("LOGNAME", pw->pw_name, 1);
		setenv("SHELL", shell, 1);
		setenv("PATH", "/usr/local/bin:/usr/bin:/usr/sbin", 1);
		setenv("XDG_RUNTIME_DIR", runtime, 1);
		setenv("WORMOS_LOGIN", "1", 1);
		/* A login shell (argv[0] starting with '-') reads /etc/profile. */
		execl(shell, "-sh", "-c", SESSION_CMD, (char *)NULL);
		_exit(127);
	}
	say("starting the desktop for %s", name);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	say("the desktop for %s ended", name);
}

int main(void)
{
	struct passwd *greeter;
	glob_t cards;
	int failures = 0;

	openlog("wormlogin", LOG_PID, LOG_AUTHPRIV);
	mkdir(RUN_DIR, 0711);
	if (access(FALLBACK, F_OK) == 0)
		console("the login screen failed before");
	if (!(greeter = getpwnam(GREETER_USER)))
		console("there is no greeter user");
	greeter = &(struct passwd){ .pw_uid = greeter->pw_uid, .pw_gid = greeter->pw_gid };
	if (glob("/dev/dri/card*", 0, NULL, &cards) != 0)
		console("there is no graphics device");
	globfree(&cards);
	if (mkdir(GREETER_HOME, 0700) < 0 && errno != EEXIST)
		console("cannot create the greeter's folder");
	if (chown(GREETER_HOME, greeter->pw_uid, greeter->pw_gid) < 0)
		console("cannot prepare the greeter's folder");

	/* The live system needs no login: its user has no password. */
	if (access("/run/wormos/live", F_OK) == 0 && getpwnam("live"))
		for (;;) {
			run_session("live");
			sleep(1);
		}

	for (;;) {
		int status = run_greeter(greeter, !users_exist());

		if (session_user[0]) {
			failures = 0;
			run_session(session_user);
			continue;
		}
		/*
		 * The greeter only ends by itself after a login, and labwc exits
		 * with status 0 when the greeter crashes: any other end failed.
		 */
		say("the login screen ended without a login (status %d)", status);
		if (++failures >= MAX_FAILURES)
			console("the login screen keeps failing");
		sleep(2);
	}
}
