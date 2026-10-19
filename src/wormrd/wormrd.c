/*
 * wormrd - the WormOS initramfs init.
 *
 * Runs as PID 1 from the initramfs. It finds the btrfs filesystem holding
 * the system, gives the snapshot tool a chance to apply pending operations
 * (such as a rewind) while nothing from the system is running yet, mounts
 * the system root read-only and hands over to the real init.
 *
 * Kernel command line:
 *
 *     root=UUID=...|LABEL=...|/dev/...   filesystem holding the system
 *     rootflags=...                       btrfs mount options (default subvol=@)
 *     rw                                  mount the root read-write
 *     init=PATH                           init to start (default /sbin/init)
 *     wormos.rescue                       open a shell before mounting the root
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/btrfs.h>

#define SYSROOT       "/sysroot"
#define TOPDIR        "/top"
#define SNAP_TOOL     "/usr/bin/snap"
#define RESCUE_SHELL  "/bin/sh"
#define ROOT_WAIT_MS  10000

/* Btrfs superblock: 64 KiB into the device. */
#define SB_OFFSET     65536
#define SB_FSID       0x20
#define SB_MAGIC      0x40
#define SB_LABEL      0x12b
#define SB_LABEL_SIZE 256
#define SB_SIZE       (SB_LABEL + SB_LABEL_SIZE)
#define BTRFS_MAGIC   "_BHRfS_M"

static char opt_root[256];
static char opt_rootflags[512] = "subvol=@";
static char opt_init[PATH_MAX] = "/sbin/init";
static int opt_rw;
static int opt_rescue;

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("wormrd: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void run_shell(void)
{
	pid_t pid;

	if (access(RESCUE_SHELL, X_OK) < 0) {
		log_msg("no rescue shell available");
		return;
	}
	log_msg("starting rescue shell, exit it to continue");
	pid = fork();
	if (pid == 0) {
		setsid();
		ioctl(0, TIOCSCTTY, 1);
		execl(RESCUE_SHELL, "sh", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
}

static __attribute__((noreturn)) void fatal(const char *fmt, ...)
{
	va_list ap;

	fputs("wormrd: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	run_shell();
	log_msg("cannot boot, rebooting in 10 seconds");
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}

static void mount_fs(const char *src, const char *dst, const char *type,
		     unsigned long flags, const char *data)
{
	mkdir(dst, 0755);
	if (mount(src, dst, type, flags, data) < 0)
		fatal("cannot mount %s: %s", dst, strerror(errno));
}

static void parse_cmdline(void)
{
	char buf[4096], *tok, *save;
	FILE *f = fopen("/proc/cmdline", "r");

	if (!f)
		fatal("cannot read /proc/cmdline: %s", strerror(errno));
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);

	for (tok = strtok_r(buf, " \t\n", &save); tok;
	     tok = strtok_r(NULL, " \t\n", &save)) {
		if (strncmp(tok, "root=", 5) == 0)
			snprintf(opt_root, sizeof(opt_root), "%s", tok + 5);
		else if (strncmp(tok, "rootflags=", 10) == 0)
			snprintf(opt_rootflags, sizeof(opt_rootflags), "%s", tok + 10);
		else if (strncmp(tok, "init=", 5) == 0)
			snprintf(opt_init, sizeof(opt_init), "%s", tok + 5);
		else if (strcmp(tok, "rw") == 0)
			opt_rw = 1;
		else if (strcmp(tok, "ro") == 0)
			opt_rw = 0;
		else if (strcmp(tok, "wormos.rescue") == 0)
			opt_rescue = 1;
	}
	if (!opt_root[0])
		fatal("no root= on the kernel command line");
}

static void format_uuid(const unsigned char *u, char *out)
{
	sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		"%02x%02x%02x%02x%02x%02x",
		u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
		u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Register a btrfs device with the kernel (needed for multi-device filesystems). */
static void btrfs_scan(const char *dev)
{
	struct btrfs_ioctl_vol_args args = { 0 };
	int fd = open("/dev/btrfs-control", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return;
	snprintf(args.name, sizeof(args.name), "%s", dev);
	ioctl(fd, BTRFS_IOC_SCAN_DEV, &args);
	close(fd);
}

/* Does the btrfs filesystem on dev match a UUID= or LABEL= spec? */
static int device_matches(const char *dev, const char *spec)
{
	unsigned char sb[SB_SIZE];
	char uuid[37], label[SB_LABEL_SIZE + 1];
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = pread(fd, sb, sizeof(sb), SB_OFFSET);
	close(fd);
	if (n != (ssize_t)sizeof(sb) || memcmp(sb + SB_MAGIC, BTRFS_MAGIC, 8) != 0)
		return 0;

	btrfs_scan(dev);
	if (strncmp(spec, "UUID=", 5) == 0) {
		format_uuid(sb + SB_FSID, uuid);
		return strcasecmp(uuid, spec + 5) == 0;
	}
	if (strncmp(spec, "LABEL=", 6) == 0) {
		memcpy(label, sb + SB_LABEL, SB_LABEL_SIZE);
		label[SB_LABEL_SIZE] = '\0';
		return strcmp(label, spec + 6) == 0;
	}
	return 0;
}

static int scan_devices(const char *spec, char *out, size_t outlen)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		char dev[PATH_MAX];

		if (e->d_name[0] == '.')
			continue;
		snprintf(dev, sizeof(dev), "/dev/%s", e->d_name);
		if (device_matches(dev, spec)) {
			snprintf(out, outlen, "%s", dev);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

/* Resolve root= to a device node, waiting for it to appear. */
static void find_root(char *dev, size_t len)
{
	struct timespec delay = { 0, 100 * 1000000L };

	for (int waited = 0; waited < ROOT_WAIT_MS; waited += 100) {
		if (opt_root[0] == '/') {
			if (access(opt_root, F_OK) == 0) {
				snprintf(dev, len, "%s", opt_root);
				btrfs_scan(dev);
				return;
			}
		} else if (scan_devices(opt_root, dev, len)) {
			return;
		}
		nanosleep(&delay, NULL);
	}
	fatal("root device %s not found", opt_root);
}

/*
 * Let the snapshot tool apply pending operations on the top-level subvolume,
 * before any program from the system root has run.
 */
static void run_snapshot_hooks(const char *dev)
{
	pid_t pid;
	int status;

	if (access(SNAP_TOOL, X_OK) < 0)
		return;
	mount_fs(dev, TOPDIR, "btrfs", MS_NOSUID | MS_NODEV, "subvolid=5");

	pid = fork();
	if (pid == 0) {
		execl(SNAP_TOOL, "snap", "--top", TOPDIR, "boot", (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0)
		log_msg("cannot run %s: %s", SNAP_TOOL, strerror(errno));
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		log_msg("snapshot boot tasks failed, booting the current system");

	if (umount(TOPDIR) < 0)
		fatal("cannot unmount %s: %s", TOPDIR, strerror(errno));
}

/* Delete everything on the initramfs to free its memory. */
static void remove_tree(int dfd, dev_t rootdev)
{
	DIR *d = fdopendir(dfd);
	struct dirent *e;

	if (!d) {
		close(dfd);
		return;
	}
	while ((e = readdir(d))) {
		struct stat st;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
		    st.st_dev != rootdev)
			continue;       /* a mount point, such as /sysroot */
		if (S_ISDIR(st.st_mode)) {
			int sub = openat(dirfd(d), e->d_name,
					 O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			if (sub >= 0)
				remove_tree(sub, rootdev);
			unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR);
		} else {
			unlinkat(dirfd(d), e->d_name, 0);
		}
	}
	closedir(d);
}

static __attribute__((noreturn)) void switch_root(void)
{
	struct stat st;
	int fd;

	umount2("/dev", MNT_DETACH);
	umount2("/sys", MNT_DETACH);
	umount2("/proc", MNT_DETACH);

	fd = open("/", O_RDONLY | O_DIRECTORY);
	if (fd >= 0 && fstat(fd, &st) == 0)
		remove_tree(fd, st.st_dev);
	else if (fd >= 0)
		close(fd);

	if (chdir(SYSROOT) < 0 ||
	    mount(".", "/", NULL, MS_MOVE, NULL) < 0 ||
	    chroot(".") < 0 || chdir("/") < 0)
		fatal("cannot switch to the root filesystem: %s", strerror(errno));

	execl(opt_init, opt_init, (char *)NULL);
	fatal("cannot start %s: %s", opt_init, strerror(errno));
}

int main(void)
{
	char dev[PATH_MAX], init[PATH_MAX + sizeof(SYSROOT)];

	if (getpid() != 1) {
		fprintf(stderr, "wormrd: must be run as PID 1\n");
		return 1;
	}

	mount_fs("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount_fs("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	setenv("PATH", "/usr/bin:/bin", 1);

	parse_cmdline();
	if (opt_rescue)
		run_shell();

	find_root(dev, sizeof(dev));
	run_snapshot_hooks(dev);

	mount_fs(dev, SYSROOT, "btrfs",
		 (opt_rw ? 0 : MS_RDONLY) | MS_NOATIME, opt_rootflags);
	snprintf(init, sizeof(init), "%s%s", SYSROOT, opt_init);
	if (access(init, X_OK) < 0)
		fatal("%s not found on the root filesystem", opt_init);

	switch_root();
}
