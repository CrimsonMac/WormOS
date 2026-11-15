/*
 * Starting a blackhole.
 *
 * bh clones a child into new user, mount, PID, network, IPC and UTS
 * namespaces and maps root inside to one unprivileged ID outside: the
 * caller's own for a normal user, a dedicated BH_ID_BASE + n for root. Root
 * also gets a cgroup limiting processes and memory.
 *
 * The child becomes the blackhole's init. Its root is a directory of the
 * blackhole's own: the host's /usr and /etc are bound in read-only, and
 * everything else (/root, /tmp, /var, /home, /run) belongs to the
 * blackhole. Host files are owned by IDs unmapped inside, so they stay
 * read-only even to root in the blackhole; the host's /home, /root, /var
 * and snapshots are not there at all. It adds a fresh /proc, a minimal
 * /dev and the inbox, pivots into the new root, brings up loopback (the
 * only network interface), applies sandbox_apply() and runs the program.
 * When the program exits, init exits and the kernel kills everything left
 * in the blackhole.
 *
 * Programs never get the host's terminal. When bh runs on one, init opens
 * a pseudo-terminal from the blackhole's own devpts and hands its master
 * to bh, which relays keys and output (relay.c): no ioctl inside reaches
 * the host terminal, and output loses the escape sequences that would set
 * the host's clipboard. Output to a terminal without input from one goes
 * through pipes for the same reason.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "bh.h"
#include "ns.h"

static pid_t child;
static char cgroup[PATH_MAX];

/* How the program's input and output reach bh (set before the clone). */
static struct {
	int pty;                /* run on a pseudo-terminal relayed by bh */
	int sock[2];            /* pty: init sends the master through it */
	int out[2], err[2];     /* otherwise: pipes for output to a terminal */
	int null_in;            /* otherwise: input from /dev/null, not a terminal */
	struct termios termios; /* pty: the host terminal's settings and size */
	struct winsize size;
} io = { .sock = { -1, -1 }, .out = { -1, -1 }, .err = { -1, -1 } };

static void kill_child(int sig)
{
	(void)sig;
	if (child > 0)
		kill(child, SIGKILL);
}

static int write_value(const char *dir, const char *file, const char *value)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	return ns_write_file(path, value);
}

static unsigned long long half_of_memory(void)
{
	unsigned long long kb = 0;
	char line[128];
	FILE *f = fopen("/proc/meminfo", "re");

	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
			break;
	if (f)
		fclose(f);
	return kb * 1024 / 2;
}

/* Limits for blackholes opened by root, so a fork bomb stays inside. */
static void cgroup_create(const char *name)
{
	char value[32];

	snprintf(cgroup, sizeof(cgroup), "%s/%s", BH_CGROUP, name);
	mkdir(BH_CGROUP, 0755);
	write_value("/sys/fs/cgroup", "cgroup.subtree_control", "+pids +memory");
	write_value(BH_CGROUP, "cgroup.subtree_control", "+pids +memory");
	if (mkdir(cgroup, 0755) < 0 && errno != EEXIST) {
		warn("cannot create %s: %s; no resource limits", cgroup, strerror(errno));
		cgroup[0] = '\0';
		return;
	}
	snprintf(value, sizeof(value), "%llu", half_of_memory());
	if (write_value(cgroup, "pids.max", "1024") < 0 ||
	    write_value(cgroup, "memory.max", value) < 0)
		warn("cannot set limits in %s: %s", cgroup, strerror(errno));
}

static void cgroup_remove(void)
{
	for (int i = 0; cgroup[0] && i < 50; i++) {
		if (rmdir(cgroup) == 0 || errno == ENOENT)
			return;
		usleep(20000);
	}
	if (cgroup[0])
		warn("cannot remove %s: %s", cgroup, strerror(errno));
}

static void fail(const char *what)
{
	fprintf(stderr, "bh: cannot %s: %s\n", what, strerror(errno));
	_exit(127);
}

static void mount_at(const char *root, const char *dst, const char *type,
		     unsigned long flags, const char *data)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s%s", root, dst);
	mkdir(path, 0755);
	if (mount(type, path, type, flags, data) < 0) {
		fprintf(stderr, "bh: cannot mount %s: %s\n", dst, strerror(errno));
		_exit(127);
	}
}

/* Device nodes are bound from the host: a user namespace cannot create any. */
static void setup_dev(const char *root)
{
	static const char *const nodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
	static const char *const links[][2] = {
		{ "pts/ptmx", "ptmx" }, { "/proc/self/fd", "fd" }, { "/proc/self/fd/0", "stdin" },
		{ "/proc/self/fd/1", "stdout" }, { "/proc/self/fd/2", "stderr" },
	};
	char src[64], dst[64], path[PATH_MAX];

	mount_at(root, "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=0755,size=64k");
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		snprintf(src, sizeof(src), "/dev/%s", nodes[i]);
		if (ns_bind(root, src, src, 0) < 0)
			fail(src);
	}
	mount_at(root, "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
		 "newinstance,ptmxmode=0666,mode=0620");
	mount_at(root, "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=64m");
	for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
		snprintf(dst, sizeof(dst), "/dev/%s", links[i][1]);
		snprintf(path, sizeof(path), "%s%s", root, dst);
		if (symlink(links[i][0], path) < 0)
			fail(dst);
	}
}

/*
 * The blackhole's root is its own and may hold anything a previous session
 * left there: make sure each mount point is a real directory, so no mount
 * follows a symlink planted in it.
 */
static void mount_point(const char *root, const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", root, name);
	if (lstat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			return;
		if (unlink(path) < 0)
			fail(name);
	}
	if (mkdir(path, 0755) < 0)
		fail(name);
}

static void loopback_up(void)
{
	struct ifreq ifr = { 0 };
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	strcpy(ifr.ifr_name, "lo");
	if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
		fail("find loopback");
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
		fail("bring up loopback");
	close(fd);
}

static void run_program(const struct box *b, char *const argv[])
{
	static char *const shell[] = { "/bin/sh", NULL };
	const char *term = getenv("TERM");
	char ps1[64];

	snprintf(ps1, sizeof(ps1), "[bh:%.32s] \\w# ", b->name);
	/* Nothing from the host's environment except the terminal type. */
	clearenv();
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin", 1);
	setenv("HOME", "/root", 1);
	setenv("USER", "root", 1);
	setenv("SHELL", "/bin/sh", 1);
	setenv("TERM", term ? term : "linux", 1);
	setenv("PS1", ps1, 1);
	setenv("BLACKHOLE", b->name, 1);
	if (chdir("/root") < 0)
		fail("enter /root");
	if (io.pty) {
		/* A session of its own, with the blackhole's terminal. */
		if (setsid() < 0 || ioctl(3, TIOCSCTTY, 0) < 0 ||
		    dup2(3, 0) < 0 || dup2(3, 1) < 0 || dup2(3, 2) < 0)
			fail("attach the terminal");
		close(3);
	}
	if (sandbox_drop_caps() < 0)
		_exit(127);
	if (!argv)
		argv = shell;
	execvp(argv[0], argv);
	fprintf(stderr, "bh: cannot run %s: %s\n", argv[0], strerror(errno));
	_exit(127);
}

/* Open the blackhole's own pseudo-terminal and send its master to bh. */
static void open_terminal(void)
{
	char ctl[CMSG_SPACE(sizeof(int))] = { 0 };
	struct iovec iov = { .iov_base = "m", .iov_len = 1 };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
			      .msg_control = ctl, .msg_controllen = sizeof(ctl) };
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	int master, slave, null;

	if ((master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0 || unlockpt(master) < 0)
		fail("open a terminal");
	if ((slave = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0)
		fail("open a terminal");
	tcsetattr(slave, TCSANOW, &io.termios);
	ioctl(slave, TIOCSWINSZ, &io.size);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &master, sizeof(int));
	if (sendmsg(io.sock[1], &msg, 0) != 1)
		fail("pass the terminal on");
	close(master);
	close(io.sock[1]);
	/* The program gets the slave as fd 3; init lets go of the host terminal. */
	if (dup2(slave, 3) < 0 || (null = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0 ||
	    dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0)
		fail("set up the terminal");
}

static void box_init(const struct box *b, char *const argv[])
{
	static const char *const points[] = { "usr", "etc", "proc", "dev", "inbox" };
	struct rlimit nproc = { 1024, 1024 };
	char root[300], inbox[300];
	int status;
	pid_t pid;

	/* Output meant for a terminal goes through bh; no terminal as input. */
	if (!io.pty && ((io.out[1] >= 0 && dup2(io.out[1], 1) < 0) ||
			(io.err[1] >= 0 && dup2(io.err[1], 2) < 0)))
		fail("set up the output");
	if (io.null_in) {
		int null = open("/dev/null", O_RDONLY | O_CLOEXEC);

		if (null < 0 || dup2(null, 0) < 0)
			fail("set up the input");
		close(null);
	}

	/* Root inside, which is b->id outside. */
	if (setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
		fail("become root in the blackhole");
	/* Changing credentials cleared it. */
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		fail("make mounts private");

	snprintf(root, sizeof(root), "%s/rootfs", b->dir);
	snprintf(inbox, sizeof(inbox), "%s/inbox", b->dir);
	/* pivot_root needs the new root to be a mount point. */
	if (mount(root, root, NULL, MS_BIND | MS_REC, NULL) < 0)
		fail("mount the blackhole's root");
	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++)
		mount_point(root, points[i]);
	/* No submounts below these, so they can be bound alone. */
	if (ns_bind(root, "/usr", "/usr", MS_RDONLY) < 0 ||
	    ns_bind(root, "/etc", "/etc", MS_RDONLY) < 0)
		fail("mount the host system");
	mount_at(root, "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	setup_dev(root);
	if (ns_bind(root, inbox, "/inbox", 0) < 0)
		fail("mount the inbox");
	if (ns_pivot(root) < 0)
		fail("enter the blackhole");

	/* The parent has moved us into our cgroup by now. */
	if (unshare(CLONE_NEWCGROUP) < 0)
		fail("create a cgroup namespace");
	if (sethostname(b->name, strlen(b->name)) < 0)
		fail("set the host name");
	loopback_up();
	if (io.pty)
		open_terminal();
	/* Nothing inside may ptrace init or take its descriptors. */
	prctl(PR_SET_DUMPABLE, 0);
	/* Fork bombs stop here, also for users' blackholes (no cgroup). */
	if (setrlimit(RLIMIT_NPROC, &nproc) < 0)
		fail("limit the processes");
	if (sandbox_apply() < 0)
		_exit(127);
	close_range(io.pty ? 4 : 3, ~0U, 0);

	if ((pid = fork()) < 0)
		fail("start the program");
	if (pid == 0)
		run_program(b, argv);
	if (io.pty)
		close(3);

	/* As init: reap everything, leave with the program. */
	for (;;) {
		pid_t done = wait(&status);

		if (done == pid)
			break;
		if (done < 0 && errno != EINTR)
			_exit(127);
	}
	_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

int box_run(const struct box *b, char *const argv[])
{
	struct sigaction kill_sa = { .sa_handler = kill_child };
	int go[2], status;
	char c = 'x';

	if (getuid() == 0) {
		/* Root's groups must not follow it in. */
		if (setgroups(0, NULL) < 0)
			die("cannot drop supplementary groups: %s", strerror(errno));
		cgroup_create(b->name);
	}
	if (pipe2(go, O_CLOEXEC) < 0)
		die("cannot create a pipe: %s", strerror(errno));
	/* Only in the terminal's foreground: a background job cannot set raw mode. */
	io.pty = isatty(0) && tcgetpgrp(0) == getpgrp() && tcgetattr(0, &io.termios) == 0;
	io.null_in = !io.pty && isatty(0);
	if (io.pty) {
		ioctl(0, TIOCGWINSZ, &io.size);
		if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, io.sock) < 0)
			die("cannot create a socket: %s", strerror(errno));
	} else if ((isatty(1) && pipe2(io.out, O_CLOEXEC) < 0) ||
		   (isatty(2) && pipe2(io.err, O_CLOEXEC) < 0)) {
		die("cannot create a pipe: %s", strerror(errno));
	}
	fflush(NULL);
	child = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET |
			CLONE_NEWIPC | CLONE_NEWUTS | SIGCHLD, NULL, NULL, NULL, NULL);
	if (child < 0)
		die("cannot create the namespaces: %s", strerror(errno));
	if (child == 0) {
		close(go[1]);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		/* EOF instead of the go byte: the parent died or gave up. */
		if (read(go[0], &c, 1) != 1)
			_exit(127);
		close(go[0]);
		box_init(b, argv);
	}

	close(go[0]);
	if (io.sock[1] >= 0)
		close(io.sock[1]);
	if (io.out[1] >= 0)
		close(io.out[1]);
	if (io.err[1] >= 0)
		close(io.err[1]);
	if (ns_map_ids(child, 0, b->id, 0, b->id) < 0) {
		warn("cannot map IDs into the blackhole: %s", strerror(errno));
		kill(child, SIGKILL);
	}
	if (cgroup[0]) {
		char pid[16];

		snprintf(pid, sizeof(pid), "%d", (int)child);
		if (write_value(cgroup, "cgroup.procs", pid) < 0)
			warn("cannot apply resource limits: %s", strerror(errno));
	}
	if (write(go[1], &c, 1) != 1)
		kill(child, SIGKILL);
	close(go[1]);

	/* The terminal's interrupts are for the program inside, not for bh. */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	sigaction(SIGTERM, &kill_sa, NULL);
	sigaction(SIGHUP, &kill_sa, NULL);
	if (io.pty)
		relay_terminal(io.sock[0], child);
	else
		relay_output(io.out[0], io.err[0]);
	while (waitpid(child, &status, 0) < 0)
		if (errno != EINTR)
			die("cannot wait for the blackhole: %s", strerror(errno));
	child = 0;
	cgroup_remove();
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
