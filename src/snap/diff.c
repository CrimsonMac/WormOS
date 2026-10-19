/*
 * Compare two directory trees and print what changed from a to b:
 *
 *     + path    added
 *     - path    removed
 *     ~ path    modified (contents, type, permissions or owner)
 *
 * Snapshots keep inode metadata, so a file whose size, times, mode and
 * owner all match is unchanged and its contents are not read.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snap.h"

static int same_time(const struct timespec *a, const struct timespec *b)
{
	return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static int same_attrs(const struct stat *a, const struct stat *b)
{
	return a->st_mode == b->st_mode && a->st_uid == b->st_uid &&
	       a->st_gid == b->st_gid;
}

static int same_content(int adfd, int bdfd, const char *name)
{
	char ba[65536], bb[65536];
	int fa, fb, same = 0;
	ssize_t na, nb;

	fa = openat(adfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	fb = openat(bdfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fa >= 0 && fb >= 0) {
		do {
			na = read(fa, ba, sizeof(ba));
			nb = read(fb, bb, sizeof(bb));
		} while (na > 0 && na == nb && memcmp(ba, bb, na) == 0);
		same = na == 0 && nb == 0;
	}
	if (fa >= 0)
		close(fa);
	if (fb >= 0)
		close(fb);
	return same;
}

/*
 * Matching metadata means unchanged. A file whose only difference is its
 * change time (for example one put back by snap restore) is compared by
 * content.
 */
static int same_file(int adfd, int bdfd, const char *name,
		     const struct stat *a, const struct stat *b)
{
	if (!same_attrs(a, b) || a->st_size != b->st_size ||
	    a->st_rdev != b->st_rdev || !same_time(&a->st_mtim, &b->st_mtim))
		return 0;
	if (same_time(&a->st_ctim, &b->st_ctim))
		return 1;
	if (S_ISLNK(a->st_mode)) {
		char ta[PATH_MAX], tb[PATH_MAX];
		ssize_t na = readlinkat(adfd, name, ta, sizeof(ta));
		ssize_t nb = readlinkat(bdfd, name, tb, sizeof(tb));

		return na >= 0 && na == nb && memcmp(ta, tb, na) == 0;
	}
	return !S_ISREG(a->st_mode) || same_content(adfd, bdfd, name);
}

static void report(char mark, const char *prefix, const char *rel,
		   int isdir, int *count)
{
	printf("%c %s%s%s\n", mark, prefix, rel, isdir ? "/" : "");
	(*count)++;
}

static void walk(int adfd, int bdfd, char *rel, size_t len,
		 const char *prefix, int *count)
{
	struct names a, b;
	size_t base = strlen(rel);
	int i = 0, j = 0;

	read_names(adfd, &a);
	read_names(bdfd, &b);

	while (i < a.n || j < b.n) {
		int c = i >= a.n ? 1 : j >= b.n ? -1 : strcmp(a.v[i], b.v[j]);
		const char *name = c <= 0 ? a.v[i] : b.v[j];
		struct stat sa, sb;

		if (base + strlen(name) + 2 > len) {
			warn("path too long, skipping: %s/%s", rel, name);
			goto next;
		}
		snprintf(rel + base, len - base, "/%s", name);

		if (c < 0) {
			fstatat(adfd, name, &sa, AT_SYMLINK_NOFOLLOW);
			report('-', prefix, rel, S_ISDIR(sa.st_mode), count);
		} else if (c > 0) {
			fstatat(bdfd, name, &sb, AT_SYMLINK_NOFOLLOW);
			report('+', prefix, rel, S_ISDIR(sb.st_mode), count);
		} else if (fstatat(adfd, name, &sa, AT_SYMLINK_NOFOLLOW) < 0 ||
			   fstatat(bdfd, name, &sb, AT_SYMLINK_NOFOLLOW) < 0) {
			warn("cannot stat %s: %s", rel, strerror(errno));
		} else if ((sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) {
			report('~', prefix, rel, 0, count);
		} else if (S_ISDIR(sa.st_mode)) {
			int fa, fb;

			if (!same_attrs(&sa, &sb))
				report('~', prefix, rel, 1, count);
			fa = openat(adfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			fb = openat(bdfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (fa >= 0 && fb >= 0)
				walk(fa, fb, rel, len, prefix, count);
			else
				warn("cannot open %s: %s", rel, strerror(errno));
			if (fa >= 0)
				close(fa);
			if (fb >= 0)
				close(fb);
		} else if (!same_file(adfd, bdfd, name, &sa, &sb)) {
			report('~', prefix, rel, 0, count);
		}
next:
		rel[base] = '\0';
		if (c <= 0)
			i++;
		if (c >= 0)
			j++;
	}
	free_names(&a);
	free_names(&b);
}

/* Print the differences between trees a and b; returns the number found. */
int diff_trees(const char *a, const char *b, const char *prefix)
{
	char rel[PATH_MAX] = "";
	int fa, fb, count = 0;

	fa = open(a, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fa < 0)
		die("cannot open %s: %s", a, strerror(errno));
	fb = open(b, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fb < 0)
		die("cannot open %s: %s", b, strerror(errno));
	walk(fa, fb, rel, sizeof(rel), prefix, &count);
	close(fa);
	close(fb);
	return count;
}
