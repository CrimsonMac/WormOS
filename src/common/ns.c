#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "ns.h"

int ns_write_file(const char *path, const char *data)
{
	size_t len = strlen(data);
	int fd = open(path, O_WRONLY | O_CLOEXEC), err;

	if (fd < 0)
		return -1;
	if (write(fd, data, len) != (ssize_t)len) {
		err = errno;
		close(fd);
		errno = err;
		return -1;
	}
	return close(fd);
}

int ns_map_ids(pid_t pid, uid_t inside_uid, uid_t outside_uid,
	       gid_t inside_gid, gid_t outside_gid)
{
	char path[64], map[64], proc[32];

	if (pid)
		snprintf(proc, sizeof(proc), "/proc/%d", (int)pid);
	else
		snprintf(proc, sizeof(proc), "/proc/self");
	snprintf(path, sizeof(path), "%s/setgroups", proc);
	if (ns_write_file(path, "deny") < 0)
		return -1;
	snprintf(path, sizeof(path), "%s/uid_map", proc);
	snprintf(map, sizeof(map), "%u %u 1\n", inside_uid, outside_uid);
	if (ns_write_file(path, map) < 0)
		return -1;
	snprintf(path, sizeof(path), "%s/gid_map", proc);
	snprintf(map, sizeof(map), "%u %u 1\n", inside_gid, outside_gid);
	return ns_write_file(path, map);
}

/* The per-mount flags of the mount holding path, as mount(2) flags. */
static unsigned long mount_flags(const char *path)
{
	static const struct { unsigned long st, ms; } map[] = {
		{ ST_NOSUID, MS_NOSUID }, { ST_NODEV, MS_NODEV },
		{ ST_NOEXEC, MS_NOEXEC }, { ST_NOATIME, MS_NOATIME },
		{ ST_NODIRATIME, MS_NODIRATIME }, { ST_RELATIME, MS_RELATIME },
	};
	struct statvfs st;
	unsigned long flags = 0;

	if (statvfs(path, &st) < 0)
		return 0;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (st.f_flag & map[i].st)
			flags |= map[i].ms;
	return flags;
}

int ns_bind(const char *root, const char *src, const char *dst, unsigned long flags)
{
	char target[PATH_MAX];
	struct stat st;

	if (stat(src, &st) < 0)
		return -1;
	if (snprintf(target, sizeof(target), "%s%s", root, dst) >= (int)sizeof(target)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		mkdir(target, 0755);
	} else {
		int fd = open(target, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);

		if (fd >= 0)
			close(fd);
	}
	if (mount(src, target, NULL, MS_BIND | MS_REC, NULL) < 0)
		return -1;
	if (flags & MS_RDONLY)
		return mount(NULL, target, NULL,
			     MS_REMOUNT | MS_BIND | MS_RDONLY | mount_flags(target), NULL);
	return 0;
}

int ns_pivot(const char *root)
{
	if (chdir(root) < 0 || syscall(SYS_pivot_root, ".", ".") < 0 ||
	    umount2(".", MNT_DETACH) < 0)
		return -1;
	return chdir("/");
}
