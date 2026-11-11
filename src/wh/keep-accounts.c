/*
 * keep-accounts ROOT - keep the running system's accounts in a changed root.
 *
 * base-files ships /etc/passwd, /etc/group and /etc/shadow with the system
 * accounts only. Upgrading it replaced the accounts of a running system
 * (the users, root's password, accounts and groups that packages and the
 * first-boot setup added). wh runs this as a hook after a transaction that
 * touched those files: each file in ROOT becomes the running system's
 * version, followed by the entries of ROOT's version whose names the
 * running system does not have (accounts the transaction added).
 *
 * Only on a running WormOS: when the running system is something else (an
 * image assembled on the build machine with wh --root), it does nothing.
 * The hook runs through ROOT's own loader, so this has no dependencies
 * beyond the C library.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const files[] = { "etc/passwd", "etc/group", "etc/shadow" };

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "re");
	char *data = NULL;
	size_t len = 0;

	if (!f)
		return NULL;
	if (getdelim(&data, &len, '\0', f) < 0) {
		free(data);
		data = strdup("");
	}
	fclose(f);
	return data;
}

static int running_wormos(void)
{
	char *release = slurp("/etc/os-release");
	int yes = release && (strstr(release, "\nID=wormos\n") || !strncmp(release, "ID=wormos\n", 10));

	free(release);
	return yes;
}

/* Does text hold a line starting with "name:"? */
static int has_name(const char *text, const char *name, size_t n)
{
	for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
		if (!strncmp(p, name, n) && p[n] == ':')
			return 1;
	return 0;
}

static int merge(const char *root, const char *rel)
{
	char live_path[PATH_MAX], path[PATH_MAX + 16], tmp[PATH_MAX + 24];
	char *live, *staged, *line, *save;
	struct stat st;
	FILE *out;
	int fd;

	snprintf(live_path, sizeof(live_path), "/%s", rel);
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if (!(live = slurp(live_path)) || !*live) {
		free(live);
		return 0;                       /* nothing to keep */
	}
	if (!(staged = slurp(path)) || stat(path, &st) < 0) {
		free(live);
		free(staged);
		return 0;
	}
	snprintf(tmp, sizeof(tmp), "%s.keep", path);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777)) < 0 ||
	    !(out = fdopen(fd, "w"))) {
		fprintf(stderr, "keep-accounts: %s: %s\n", tmp, strerror(errno));
		return 1;
	}
	fputs(live, out);
	if (live[strlen(live) - 1] != '\n')
		fputc('\n', out);
	for (line = strtok_r(staged, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		size_t n = strcspn(line, ":");

		if (line[n] == ':' && !has_name(live, line, n))
			fprintf(out, "%s\n", line);
	}
	if (fchown(fd, st.st_uid, st.st_gid) < 0 || fchmod(fd, st.st_mode & 07777) < 0 ||
	    fflush(out) != 0 || fsync(fd) < 0 || fclose(out) != 0 || rename(tmp, path) < 0) {
		fprintf(stderr, "keep-accounts: %s: %s\n", path, strerror(errno));
		unlink(tmp);
		return 1;
	}
	free(live);
	free(staged);
	return 0;
}

int main(int argc, char **argv)
{
	char real_root[PATH_MAX];
	int status = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: keep-accounts ROOT\n");
		return 2;
	}
	if (!realpath(argv[1], real_root) || !strcmp(real_root, "/") || !running_wormos())
		return 0;
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		status |= merge(real_root, files[i]);
	return status;
}
