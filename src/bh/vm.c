/*
 * Virtual machine blackholes (bh open --vm): programs run on a kernel of
 * their own, so a kernel exploit takes over only the guest.
 *
 * The blackhole starts as usual (namespaces, cgroup, Landlock, seccomp,
 * pasta with --net), but its first session's program is the virtual
 * machine monitor, kvmtool's lkvm (static), and the root that programs
 * would have seen becomes the guest's root file system over virtio-9p.
 * The monitor itself is jailed: even taking it over from the guest lands
 * in an unprivileged, sandboxed blackhole with no host files but the
 * read-only system.
 *
 * The jail is a tmpfs of its own (vm_jail):
 *
 *     /share          the blackhole's root (its /usr and /etc read-only),
 *                     the only directory the guest can see
 *     /dev            null, zero, urandom..., kvm and a devpts instance
 *     /kernel         the host's kernel, read-only (/boot/vmlinuz)
 *     /lkvm           the monitor, read-only
 *     /etc/resolv.conf  the name server for the monitor's user-mode network
 *     /tmp            the monitor's $HOME
 *
 * There is no /proc: a 9p server following a symlink there could reach
 * the monitor's own memory. The guest's init is BH_LIBDIR/vm-init, bound
 * at /run/bh-init in the blackhole (vm-init.c). It gets the command, the
 * terminal's size and type and whether there is a network from the
 * kernel command line, and leaves the program's exit code in
 * /run/bh-status. The console is a virtio console on the session's
 * terminal; Ctrl+A X stops the machine.
 *
 * With --net, lkvm's user-mode network turns the guest's connections into
 * sockets of the monitor, inside the blackhole's network namespace, so
 * they go through pasta and the same routes as a namespace blackhole's.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bh.h"
#include "ns.h"

#define LKVM   "/usr/bin/lkvm"
#define KERNEL "/boot/vmlinuz"
#define JAIL   "/tmp"           /* where the jail is put together (private) */

int vm_check(void)
{
	struct stat st;
	int fd;

	if (stat(LKVM, &st) < 0) {
		warn("--vm needs kvmtool (%s): install the kvmtool package", LKVM);
		return -1;
	}
	if (stat(KERNEL, &st) < 0) {
		warn("--vm needs a kernel at %s", KERNEL);
		return -1;
	}
	if ((fd = open("/dev/kvm", O_RDWR | O_CLOEXEC)) < 0) {
		if (errno == ENOENT)
			warn("--vm needs hardware virtualisation (KVM), which this machine does not "
			     "offer; in a virtual machine, turn on nested virtualisation");
		else
			warn("cannot open /dev/kvm: %s", strerror(errno));
		return -1;
	}
	close(fd);
	return 0;
}

void vm_jail(const char *root, int net)
{
	char path[PATH_MAX];
	int fd;

	if (mount("tmpfs", JAIL, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=16m") < 0)
		fail("mount the monitor's root");
	snprintf(path, sizeof(path), "%s/share", JAIL);
	if (mkdir(path, 0755) < 0 || mount(root, path, NULL, MS_BIND | MS_REC, NULL) < 0)
		fail("share the blackhole's root");
	/* A code from an earlier run must not pass for this one's. */
	snprintf(path, sizeof(path), "%s/share/run/bh-status", JAIL);
	unlink(path);
	setup_dev(JAIL, 1);
	if (ns_bind(JAIL, KERNEL, "/kernel", MS_RDONLY) < 0)
		fail("mount the kernel");
	if (ns_bind(JAIL, LKVM, "/lkvm", MS_RDONLY) < 0)
		fail("mount the monitor");
	snprintf(path, sizeof(path), "%s/etc", JAIL);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/etc/resolv.conf", JAIL);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0)
		fail("write the monitor's resolv.conf");
	if (net)
		dprintf(fd, "nameserver %s\n", BH_NET_DNS);
	close(fd);
	snprintf(path, sizeof(path), "%s/tmp", JAIL);
	mkdir(path, 01777);
	if (ns_pivot(JAIL) < 0)
		fail("enter the monitor's root");
}

static void append(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void append(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

/* lkvm's command line for this session (in the session's process). */
char **vm_argv(const struct box *b, const struct opts *o, const struct request *req,
	       char **argv)
{
	static char cmdline[2048], mem[16], cpus[16], name[48];
	static char *args[32];
	int n = 0, len = 0;

	snprintf(mem, sizeof(mem), "%u", o->vm_mb);
	snprintf(cpus, sizeof(cpus), "%u", o->cpus);
	snprintf(name, sizeof(name), "bh-%s", b->name);
	snprintf(cmdline, sizeof(cmdline),
		 "console=hvc0 root=bhroot rootfstype=9p rw "
		 "rootflags=trans=virtio,version=9p2000.L,cache=loose,msize=262144 "
		 "init=/run/bh-init quiet loglevel=1 panic=-1 slab_nomerge pci=conf1 8250.nr_uarts=0 "
		 "bh.name=%s bh.net=%d bh.rows=%u bh.cols=%u bh.term=",
		 b->name, o->net, req->size.ws_row, req->size.ws_col);
	/* The terminal type and the command, hex-encoded: no spaces or quotes. */
	for (const char *p = req->term; *p; p++)
		append(cmdline, sizeof(cmdline), "%02x", (unsigned char)*p);
	append(cmdline, sizeof(cmdline), " bh.cmd=");
	for (int i = 0; argv && argv[i]; i++) {
		for (const char *p = argv[i]; ; p++) {
			append(cmdline, sizeof(cmdline), "%02x", (unsigned char)*p);
			if (!*p)
				break;
		}
	}
	len = strlen(cmdline);
	if (len >= (int)sizeof(cmdline) - 1) {
		fprintf(stderr, "bh: the command is too long for a virtual machine\n");
		_exit(127);
	}

	args[n++] = "/lkvm";
	args[n++] = "run";
	args[n++] = "--nodefaults";
	args[n++] = "--loglevel";
	args[n++] = "error";
	args[n++] = "--name";
	args[n++] = name;
	args[n++] = "--kernel";
	args[n++] = "/kernel";
	args[n++] = "--mem";
	args[n++] = mem;
	args[n++] = "--cpus";
	args[n++] = cpus;
	args[n++] = "--console";
	args[n++] = "virtio";
	args[n++] = "--rng";
	args[n++] = "--9p";
	args[n++] = "/share,bhroot";
	args[n++] = "--network";
	args[n++] = o->net ? "mode=user" : "mode=none";
	args[n++] = "--params";
	args[n++] = cmdline;
	args[n] = NULL;
	return args;
}

/* The guest's exit code for its program, from /run/bh-status. */
int vm_status(void)
{
	char buf[16] = "";
	int fd = open("/share/run/bh-status", O_RDONLY | O_NOFOLLOW | O_CLOEXEC), code;

	if (fd < 0)
		return 1;
	if (read(fd, buf, sizeof(buf) - 1) < 0)
		buf[0] = '\0';
	close(fd);
	code = atoi(buf);
	return code >= 0 && code <= 255 ? code : 1;
}
