/*
 * worm - WormOS system features.
 *
 *     worm unlock             make the system writable for manual changes
 *     worm lock [-c TEXT]     make it read-only again and snapshot the changes
 *     worm fw ...             the firewall (see fw.c)
 *
 * Only the mount of / changes (a bind remount), never the filesystem, so
 * /var and /home are not affected. Unlocking is a guard against accidents,
 * not a security boundary: root can always unlock.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "worm.h"

void die(const char *fmt, const char *arg)
{
	fprintf(stderr, "worm: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	exit(1);
}

int run(char *const argv[], char *out, size_t len, int quiet)
{
	int pipefd[2], status;
	size_t n = 0;
	pid_t pid;

	if (out && pipe(pipefd) < 0)
		return -1;
	fflush(stdout);
	if ((pid = fork()) < 0)
		return -1;
	if (pid == 0) {
		if (out) {
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[0]);
			close(pipefd[1]);
		}
		if (quiet) {
			int null = open("/dev/null", O_WRONLY);

			if (null >= 0)
				dup2(null, STDERR_FILENO);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	if (out) {
		char discard[512];
		ssize_t r;

		close(pipefd[1]);
		while (n < len - 1 && (r = read(pipefd[0], out + n, len - 1 - n)) > 0)
			n += r;
		out[n] = '\0';
		/* Drain the rest, or the command may die of SIGPIPE. */
		while (read(pipefd[0], discard, sizeof(discard)) > 0)
			;
		close(pipefd[0]);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The current per-mount flags of /, as mount(2) flags. */
static unsigned long root_flags(int *readonly)
{
	static const struct { unsigned long st, ms; } map[] = {
		{ ST_NOSUID, MS_NOSUID }, { ST_NODEV, MS_NODEV },
		{ ST_NOEXEC, MS_NOEXEC }, { ST_NOATIME, MS_NOATIME },
		{ ST_NODIRATIME, MS_NODIRATIME }, { ST_RELATIME, MS_RELATIME },
	};
	struct statvfs st;
	unsigned long flags = 0;

	if (statvfs("/", &st) < 0)
		die("cannot read the flags of /: %s", strerror(errno));
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (st.f_flag & map[i].st)
			flags |= map[i].ms;
	*readonly = (st.f_flag & ST_RDONLY) != 0;
	return flags;
}

/* The mount ID of /, from mountinfo. */
static int root_mount_id(void)
{
	char line[4096], mnt[PATH_MAX];
	FILE *f = fopen("/proc/self/mountinfo", "re");
	int id, found = -1;

	if (!f)
		return -1;
	while (found < 0 && fgets(line, sizeof(line), f))
		if (sscanf(line, "%d %*s %*s %*s %4095s", &id, mnt) == 2 &&
		    strcmp(mnt, "/") == 0)
			found = id;
	fclose(f);
	return found;
}

/* List processes holding a file on / open for writing. */
static void show_writers(void)
{
	int root_id = root_mount_id(), shown = 0;
	struct dirent *p;
	DIR *proc = opendir("/proc");

	while (proc && (p = readdir(proc))) {
		char dir[300], path[PATH_MAX], target[PATH_MAX], comm[64] = "?";
		struct dirent *fd;
		DIR *fds;
		FILE *f;

		if (p->d_name[0] < '0' || p->d_name[0] > '9')
			continue;
		snprintf(dir, sizeof(dir), "/proc/%s/fd", p->d_name);
		if (!(fds = opendir(dir)))
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", p->d_name);
		if ((f = fopen(path, "re"))) {
			if (fgets(comm, sizeof(comm), f))
				comm[strcspn(comm, "\n")] = '\0';
			fclose(f);
		}
		while ((fd = readdir(fds))) {
			char line[256];
			int flags = -1, mnt = -1;
			ssize_t n;

			if (fd->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "/proc/%s/fdinfo/%s", p->d_name, fd->d_name);
			if (!(f = fopen(path, "re")))
				continue;
			while (fgets(line, sizeof(line), f)) {
				sscanf(line, "flags: %o", (unsigned *)&flags);
				sscanf(line, "mnt_id: %d", &mnt);
			}
			fclose(f);
			if (mnt != root_id || flags < 0 || (flags & O_ACCMODE) == O_RDONLY)
				continue;
			snprintf(path, sizeof(path), "%s/%s", dir, fd->d_name);
			n = readlink(path, target, sizeof(target) - 1);
			target[n > 0 ? n : 0] = '\0';
			printf("  %s (pid %s): %s\n", comm, p->d_name, target);
			shown++;
		}
		closedir(fds);
	}
	if (proc)
		closedir(proc);
	if (!shown)
		printf("  (none found; a program may have a file mapped for writing)\n");
}

/* Changes to / made now would be lost when a staged package change applies. */
static void refuse_if_pending(void)
{
	char status[128] = "";

	if (run((char *[]){ "snap", "stage", "status", NULL }, status, sizeof(status), 0) == 0 &&
	    strstr(status, "pending"))
		die("a package change is waiting to be applied; restart first%s", "");
}

int root_begin_change(void)
{
	int ro;
	unsigned long flags = root_flags(&ro);

	refuse_if_pending();
	if (!ro)
		return 0;
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | flags, NULL) < 0)
		die("cannot make / writable: %s", strerror(errno));
	return 1;
}

void root_end_change(int relock)
{
	int ro;
	unsigned long flags = root_flags(&ro);

	if (!relock || ro)
		return;
	sync();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | flags, NULL) < 0)
		fprintf(stderr, "worm: / stays writable (%s); run 'worm lock'\n",
			strerror(errno));
}

static int cmd_unlock(void)
{
	int ro;
	unsigned long flags = root_flags(&ro);

	if (!ro) {
		printf("The system is already unlocked.\n");
		return 0;
	}
	refuse_if_pending();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | flags, NULL) < 0)
		die("cannot unlock: %s", strerror(errno));
	printf("The system is unlocked: / is writable.\n"
	       "Run 'worm lock' when you are done; your changes are then snapshotted.\n");
	return 0;
}

static int cmd_lock(const char *comment)
{
	char out[256];
	int ro;
	unsigned long flags = root_flags(&ro);

	if (ro) {
		printf("The system is already locked.\n");
		return 0;
	}
	sync();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | flags, NULL) < 0) {
		if (errno != EBUSY)
			die("cannot lock: %s", strerror(errno));
		printf("Cannot lock yet: these programs have files open for writing:\n");
		show_writers();
		printf("Close them and run 'worm lock' again.\n");
		return 1;
	}
	printf("The system is locked.\n");
	if (run((char *[]){ "snap", "now", "--auto", "-c", (char *)comment, NULL },
		out, sizeof(out), 0) == 0 && out[0])
		printf("%s", out);
	return 0;
}

static void usage(FILE *out)
{
	fputs("usage: worm unlock | lock [-c COMMENT]\n"
	      "       worm fw status | allow PORT[/tcp|/udp] | deny PORT[/tcp|/udp] | load\n"
	      "PORT is a number or a range such as 6000-6010; tcp is the default.\n", out);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
		usage(stdout);
		return 0;
	}
	if (geteuid() != 0)
		die("this needs root, try again with su%s", "");
	if (!strcmp(argv[1], "fw")) {
		int r = cmd_fw(argc - 2, argv + 2);

		if (r == 2)
			usage(stderr);
		return r;
	}
	if (!strcmp(argv[1], "unlock") && argc == 2)
		return cmd_unlock();
	if (!strcmp(argv[1], "lock") && argc == 2)
		return cmd_lock("Manual changes");
	if (!strcmp(argv[1], "lock") && argc == 4 && !strcmp(argv[2], "-c"))
		return cmd_lock(argv[3]);
	usage(stderr);
	return 2;
}
