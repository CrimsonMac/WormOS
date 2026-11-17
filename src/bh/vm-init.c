/*
 * vm-init - init of the guest in a virtual machine blackhole (bh open --vm).
 *
 * Linked statically, so it runs whatever /usr the blackhole has (--from).
 * The guest's root is the blackhole's root over 9p (see vm.c). vm-init
 * mounts /proc, /sys, /dev (the guest kernel's own devtmpfs), /dev/pts and
 * /dev/shm, sets the host name, brings up loopback and, with bh.net=1, the
 * network of lkvm's user-mode stack (192.168.33.15/24 through
 * 192.168.33.1). It runs the command from bh.cmd (or a shell) as root on
 * the console (hvc0) with the terminal type and size from bh.term,
 * bh.rows and bh.cols and the same clean environment as a namespace
 * blackhole, waits for it, writes its exit code to /run/bh-status and
 * resets the machine, which ends the monitor (kvmtool has no ACPI, so a
 * power-off would only halt).
 *
 * Kernel parameters: bh.name, bh.net, bh.rows, bh.cols, bh.term (hex),
 * bh.cmd (hex, each argument ending in a NUL byte).
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define GUEST_ADDR "192.168.33.15"
#define GUEST_MASK "255.255.255.0"
#define GUEST_GW   "192.168.33.1"

static char cmdline[4096];
static char *args[256];
static char term[64] = "linux";

static void say(const char *what)
{
	dprintf(2, "bh vm-init: cannot %s: %s\n", what, strerror(errno));
}

static void __attribute__((noreturn)) finish(int code)
{
	int fd = open("/run/bh-status", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);

	if (fd >= 0) {
		dprintf(fd, "%d\n", code);
		close(fd);
	}
	sync();
	/* Without ACPI a power-off only halts; kvmtool ends on a reset. */
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}

static const char *param(const char *key)
{
	size_t n = strlen(key);

	for (char *p = cmdline; p && *p; ) {
		while (*p == ' ')
			p++;
		if (!strncmp(p, key, n) && p[n] == '=')
			return p + n + 1;
		p = strchr(p, ' ');
	}
	return NULL;
}

/* Decode hex up to the next space into out; returns the length. */
static size_t unhex(const char *in, char *out, size_t size)
{
	size_t n = 0;

	while (in && in[0] && in[0] != ' ' && in[1] && n < size) {
		unsigned v;

		if (sscanf(in, "%2x", &v) != 1)
			break;
		out[n++] = v;
		in += 2;
	}
	return n;
}

static void mount_fs(const char *type, const char *dir, unsigned long flags, const char *data)
{
	mkdir(dir, 0755);
	if (mount(type, dir, type, flags, data) < 0 && errno != EBUSY)
		say(dir);
}

static void set_addr(int fd, const char *ifname, unsigned long req, const char *addr)
{
	struct ifreq ifr = { 0 };
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	sin->sin_family = AF_INET;
	inet_pton(AF_INET, addr, &sin->sin_addr);
	if (ioctl(fd, req, &ifr) < 0)
		say("configure the network");
}

static void link_up(int fd, const char *ifname)
{
	struct ifreq ifr = { 0 };

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
		return;
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
		say("bring up an interface");
}

static void network(int net)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct rtentry rt = { 0 };

	if (fd < 0)
		return;
	link_up(fd, "lo");
	if (net) {
		set_addr(fd, "eth0", SIOCSIFADDR, GUEST_ADDR);
		set_addr(fd, "eth0", SIOCSIFNETMASK, GUEST_MASK);
		link_up(fd, "eth0");
		((struct sockaddr_in *)&rt.rt_dst)->sin_family = AF_INET;
		((struct sockaddr_in *)&rt.rt_genmask)->sin_family = AF_INET;
		((struct sockaddr_in *)&rt.rt_gateway)->sin_family = AF_INET;
		inet_pton(AF_INET, GUEST_GW, &((struct sockaddr_in *)&rt.rt_gateway)->sin_addr);
		rt.rt_flags = RTF_UP | RTF_GATEWAY;
		if (ioctl(fd, SIOCADDRT, &rt) < 0)
			say("add the default route");
	}
	close(fd);
}

int main(void)
{
	static char cmd[2048];
	const char *v;
	char name[64] = "blackhole", ps1[96];
	struct winsize ws = { 0 };
	int fd, status, net;
	size_t n;
	pid_t pid, done;

	if (getpid() != 1)
		return 1;
	mount_fs("proc", "/proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount_fs("sysfs", "/sys", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount_fs("devtmpfs", "/dev", MS_NOSUID, "mode=0755");
	mount_fs("devpts", "/dev/pts", MS_NOSUID | MS_NOEXEC, "mode=0620,ptmxmode=0666");
	mount_fs("tmpfs", "/dev/shm", MS_NOSUID | MS_NODEV, "mode=1777");

	if ((fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC)) >= 0) {
		ssize_t r = read(fd, cmdline, sizeof(cmdline) - 1);

		cmdline[r > 0 ? r : 0] = '\0';
		close(fd);
		cmdline[strcspn(cmdline, "\n")] = '\0';
	}
	if ((v = param("bh.name")))
		snprintf(name, sizeof(name), "%.*s", (int)strcspn(v, " "), v);
	net = (v = param("bh.net")) && v[0] == '1';
	if ((v = param("bh.rows")))
		ws.ws_row = atoi(v);
	if ((v = param("bh.cols")))
		ws.ws_col = atoi(v);
	if ((v = param("bh.term")) && (n = unhex(v, term, sizeof(term) - 1)) > 0)
		term[n] = '\0';
	if ((v = param("bh.cmd")) && (n = unhex(v, cmd, sizeof(cmd) - 1)) > 0) {
		int argc = 0;

		cmd[n] = '\0';
		for (char *p = cmd; p < cmd + n && argc < 255; p += strlen(p) + 1)
			args[argc++] = p;
		args[argc] = NULL;
	}

	if (sethostname(name, strlen(name)) < 0)
		say("set the host name");
	network(net);

	if ((pid = fork()) < 0)
		finish(127);
	if (pid == 0) {
		if (setsid() < 0 || (fd = open("/dev/hvc0", O_RDWR)) < 0 ||
		    ioctl(fd, TIOCSCTTY, 0) < 0 ||
		    dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {
			say("open the console");
			_exit(127);
		}
		if (fd > 2)
			close(fd);
		if (ws.ws_row && ws.ws_col)
			ioctl(0, TIOCSWINSZ, &ws);
		snprintf(ps1, sizeof(ps1), "[bh:%s vm] \\w# ", name);
		clearenv();
		setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin", 1);
		setenv("HOME", "/root", 1);
		setenv("USER", "root", 1);
		setenv("SHELL", "/bin/sh", 1);
		setenv("TERM", term, 1);
		setenv("PS1", ps1, 1);
		setenv("BLACKHOLE", name, 1);
		if (chdir("/root") < 0 && chdir("/") < 0)
			_exit(127);
		if (!args[0]) {
			args[0] = "/bin/sh";
			args[1] = NULL;
		}
		execvp(args[0], args);
		dprintf(2, "bh: cannot run %s: %s\n", args[0], strerror(errno));
		_exit(127);
	}
	/* As init: reap everything, power off when the program ends. */
	for (;;) {
		done = wait(&status);
		if (done == pid)
			break;
		if (done < 0 && errno != EINTR)
			finish(127);
	}
	kill(-1, SIGKILL);
	finish(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}
