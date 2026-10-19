/* Small helpers shared by the snap commands. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snap.h"

void die(const char *fmt, ...)
{
	va_list ap;

	fputs("snap: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void warn(const char *fmt, ...)
{
	va_list ap;

	fputs("snap: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void path_join(char *out, size_t len, const char *a, const char *b)
{
	if ((size_t)snprintf(out, len, "%s/%s", a, b) >= len)
		die("path too long: %s/%s", a, b);
}

const char *scope_name(int scope)
{
	switch (scope) {
	case SCOPE_SYSTEM: return "system";
	case SCOPE_HOME:   return "home";
	case SCOPE_ALL:    return "all";
	default:           return "none";
	}
}

/* For messages: "system", "home" or "system and home". */
const char *scope_desc(int scope)
{
	return scope == SCOPE_ALL ? "system and home" : scope_name(scope);
}

const char *scope_path(int scope)
{
	return scope == SCOPE_HOME ? "/home" : "/";
}

const char *scope_subvol(int scope)
{
	return scope == SCOPE_HOME ? SUBVOL_HOME : SUBVOL_SYSTEM;
}

const char *scope_dir(int scope)
{
	return scope == SCOPE_HOME ? SNAP_HOME : SNAP_SYSTEM;
}

/* Remove name (a file or a whole directory tree) relative to dfd. */
int remove_tree_at(int dfd, const char *name)
{
	struct stat st;
	struct dirent *e;
	DIR *d;
	int fd, ret = 0;

	if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlinkat(dfd, name, 0);

	fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0 || !(d = fdopendir(fd))) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
		    remove_tree_at(dirfd(d), e->d_name) < 0)
			ret = -1;
	}
	closedir(d);
	if (unlinkat(dfd, name, AT_REMOVEDIR) < 0)
		ret = -1;
	return ret;
}
