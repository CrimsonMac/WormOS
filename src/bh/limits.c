/*
 * Limits for a blackhole: a cgroup BH_CGROUP/UID-NAME holding its init and
 * everything inside, with
 *
 *     pids.max     BH_PIDS_DEFAULT tasks (threads count), --pids N
 *     memory.max   half of the memory, --memory SIZE (with --vm: the
 *                  guest's memory and some room for the monitor)
 *     cpu.weight   BH_CPU_WEIGHT, half of everything else's, so a busy
 *                  blackhole cannot make the desktop sluggish
 *
 * Root writes the cgroup itself. A user cannot move a process into a
 * cgroup, so bh runs the setuid helper BH_LIBDIR/limits (limits-helper.c),
 * which only acts on a direct child of its caller that belongs to the
 * caller, and only on cgroups named after the caller's ID. Without a
 * cgroup (no helper), RLIMIT_NPROC still caps the processes.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bh.h"

static int write_value(const char *dir, const char *file, const char *value)
{
	char path[PATH_MAX];
	int fd;
	ssize_t r;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if ((fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW)) < 0)
		return -1;
	r = write(fd, value, strlen(value));
	close(fd);
	return r == (ssize_t)strlen(value) ? 0 : -1;
}

unsigned long long memory_total(void)
{
	unsigned long long kb = 0;
	char line[128];
	FILE *f = fopen("/proc/meminfo", "re");

	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
			break;
	if (f)
		fclose(f);
	return kb * 1024;
}

int cgroup_create(uid_t uid, const char *name, pid_t pid, unsigned long long memory,
		  unsigned pids)
{
	char dir[PATH_MAX], value[32];

	if (!valid_name(name)) {
		errno = EINVAL;
		return -1;
	}
	mkdir(BH_CGROUP, 0755);
	/* Controllers this kernel lacks are left out one by one. */
	write_value("/sys/fs/cgroup", "cgroup.subtree_control", "+pids");
	write_value("/sys/fs/cgroup", "cgroup.subtree_control", "+memory");
	write_value("/sys/fs/cgroup", "cgroup.subtree_control", "+cpu");
	write_value(BH_CGROUP, "cgroup.subtree_control", "+pids");
	write_value(BH_CGROUP, "cgroup.subtree_control", "+memory");
	write_value(BH_CGROUP, "cgroup.subtree_control", "+cpu");
	snprintf(dir, sizeof(dir), "%s/%u-%s", BH_CGROUP, (unsigned)uid, name);
	/*
	 * Left over from a bh that was killed: the blackhole is not running
	 * (bh holds its lock), so whatever is still in there goes.
	 */
	if (mkdir(dir, 0755) < 0) {
		if (errno != EEXIST || cgroup_remove(uid, name) < 0 || mkdir(dir, 0755) < 0)
			return -1;
	}
	snprintf(value, sizeof(value), "%u", pids);
	if (write_value(dir, "pids.max", value) < 0)
		return -1;
	snprintf(value, sizeof(value), "%llu", memory);
	if (write_value(dir, "memory.max", value) < 0)
		return -1;
	/* No swapping a blackhole's memory out to disk. */
	write_value(dir, "memory.swap.max", "0");
	write_value(dir, "cpu.weight", BH_CPU_WEIGHT);
	snprintf(value, sizeof(value), "%d", (int)pid);
	return write_value(dir, "cgroup.procs", value);
}

int cgroup_remove(uid_t uid, const char *name)
{
	char dir[PATH_MAX];

	if (!valid_name(name)) {
		errno = EINVAL;
		return -1;
	}
	snprintf(dir, sizeof(dir), "%s/%u-%s", BH_CGROUP, (unsigned)uid, name);
	/* The last processes may take a moment to go. */
	for (int i = 0; i < 50; i++) {
		if (rmdir(dir) == 0 || errno == ENOENT)
			return 0;
		write_value(dir, "cgroup.kill", "1");
		usleep(20000);
	}
	return -1;
}

static int helper(char *const argv[])
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execv(BH_LIBDIR "/limits", argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int limits_apply(const struct box *b, const struct opts *o, pid_t child)
{
	char spid[16], smem[32], spids[16];

	if (getuid() == 0) {
		if (cgroup_create(0, b->name, child, o->memory, o->pids) == 0)
			return 0;
		warn("cannot set up the limits of %s: %s; processes are still limited",
		     b->name, strerror(errno));
		return -1;
	}
	snprintf(spid, sizeof(spid), "%d", (int)child);
	snprintf(smem, sizeof(smem), "%llu", o->memory);
	snprintf(spids, sizeof(spids), "%u", o->pids);
	if (helper((char *const[]){ "limits", "create", (char *)b->name, spid, smem, spids,
				    NULL }) == 0)
		return 0;
	warn("cannot set up the limits of %s; processes are still limited", b->name);
	return -1;
}

void limits_remove(const struct box *b)
{
	if (getuid() == 0) {
		if (cgroup_remove(0, b->name) < 0)
			warn("cannot remove the cgroup of %s: %s", b->name, strerror(errno));
		return;
	}
	if (helper((char *const[]){ "limits", "remove", (char *)b->name, NULL }) < 0)
		warn("cannot remove the cgroup of %s", b->name);
}
