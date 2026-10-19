/*
 * snap restore: bring back one file or directory from a snapshot without
 * rewinding everything.
 *
 * The copy is made with reflinks, so it is instant and shares data with the
 * snapshot. It is built under a temporary name and swapped into place
 * atomically. The state before the restore is snapshotted first.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/openat2.h>

#include "snap.h"

/* Paths outside the snapshots, refused by restore. */
static const char *const unsnapshotted[] = {
	"/var", "/root", "/proc", "/sys", "/dev", "/run", "/tmp", NULL
};

static int under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Make path absolute and clean, rejecting "..". */
static void normalize(const char *in, char *out, size_t len)
{
	char buf[PATH_MAX], *tok, *save;
	size_t n = 0;

	if (in[0] == '/') {
		snprintf(buf, sizeof(buf), "%s", in);
	} else {
		char cwd[PATH_MAX];

		if (!getcwd(cwd, sizeof(cwd)))
			die("cannot get the current directory: %s", strerror(errno));
		if ((size_t)snprintf(buf, sizeof(buf), "%s/%s", cwd, in) >= sizeof(buf))
			die("path too long: %s", in);
	}

	out[0] = '\0';
	for (tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (strcmp(tok, ".") == 0)
			continue;
		if (strcmp(tok, "..") == 0)
			die("paths with '..' are not supported: %s", in);
		n += snprintf(out + n, len - n, "/%s", tok);
		if (n >= len)
			die("path too long: %s", in);
	}
	if (!out[0])
		snprintf(out, len, "/");
}

/* Open a directory inside root, resolving symlinks as if root were "/". */
static int open_in_root(int rootfd, const char *rel)
{
	struct open_how how = {
		.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
		.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,
	};

	if (!*rel)
		return dup(rootfd);
	return syscall(SYS_openat2, rootfd, rel, &how, sizeof(how));
}

static void copy_xattrs(int sfd, int dfd)
{
	char names[4096], value[65536];
	ssize_t n = flistxattr(sfd, names, sizeof(names));

	for (ssize_t i = 0; i < n; i += strlen(names + i) + 1) {
		ssize_t v = fgetxattr(sfd, names + i, value, sizeof(value));

		if (v >= 0 && fsetxattr(dfd, names + i, value, v, 0) < 0)
			warn("cannot copy attribute %s: %s", names + i, strerror(errno));
	}
}

static int copy_data(int sfd, int dfd, off_t size)
{
	if (ioctl(dfd, FICLONE, sfd) == 0)
		return 0;
	while (size > 0) {
		ssize_t n = copy_file_range(sfd, NULL, dfd, NULL, size, 0);

		if (n <= 0)
			return -1;
		size -= n;
	}
	return 0;
}

/* Finish a copied inode: owner first (it clears set-ID bits), then the rest. */
static int set_meta(int fd, const struct stat *st, int sfd)
{
	struct timespec times[2] = { st->st_atim, st->st_mtim };
	int ret = 0;

	if (fchown(fd, st->st_uid, st->st_gid) < 0)
		ret = -1;
	if (sfd >= 0)
		copy_xattrs(sfd, fd);
	if (fchmod(fd, st->st_mode & 07777) < 0 || futimens(fd, times) < 0)
		ret = -1;
	return ret;
}

static int copy_at(int sdir, const char *sname, int ddir, const char *dname);

static int copy_dir(int sdir, const char *sname, int ddir, const char *dname,
		    const struct stat *st)
{
	int sfd, dfd, ret = 0;
	struct dirent *e;
	DIR *d;

	if (mkdirat(ddir, dname, 0700) < 0)
		return -1;
	sfd = openat(sdir, sname, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	dfd = openat(ddir, dname, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (sfd < 0 || dfd < 0 || !(d = fdopendir(dup(sfd)))) {
		ret = -1;
		goto out;
	}
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
		    copy_at(dirfd(d), e->d_name, dfd, e->d_name) < 0)
			ret = -1;
	}
	closedir(d);
	if (set_meta(dfd, st, sfd) < 0)
		ret = -1;
out:
	if (sfd >= 0)
		close(sfd);
	if (dfd >= 0)
		close(dfd);
	return ret;
}

static int copy_file(int sdir, const char *sname, int ddir, const char *dname,
		     const struct stat *st)
{
	int sfd, dfd, ret;

	sfd = openat(sdir, sname, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (sfd < 0)
		return -1;
	dfd = openat(ddir, dname, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (dfd < 0) {
		close(sfd);
		return -1;
	}
	ret = copy_data(sfd, dfd, st->st_size);
	if (set_meta(dfd, st, sfd) < 0)
		ret = -1;
	close(sfd);
	if (close(dfd) < 0)
		ret = -1;
	return ret;
}

static int copy_at(int sdir, const char *sname, int ddir, const char *dname)
{
	struct timespec times[2];
	char target[PATH_MAX];
	struct stat st;
	ssize_t n;
	int ret;

	if (fstatat(sdir, sname, &st, AT_SYMLINK_NOFOLLOW) < 0)
		return -1;
	times[0] = st.st_atim;
	times[1] = st.st_mtim;

	switch (st.st_mode & S_IFMT) {
	case S_IFDIR:
		ret = copy_dir(sdir, sname, ddir, dname, &st);
		break;
	case S_IFREG:
		ret = copy_file(sdir, sname, ddir, dname, &st);
		break;
	case S_IFLNK:
		n = readlinkat(sdir, sname, target, sizeof(target) - 1);
		if (n < 0)
			return -1;
		target[n] = '\0';
		ret = symlinkat(target, ddir, dname);
		if (ret == 0 &&
		    (fchownat(ddir, dname, st.st_uid, st.st_gid, AT_SYMLINK_NOFOLLOW) < 0 ||
		     utimensat(ddir, dname, times, AT_SYMLINK_NOFOLLOW) < 0))
			ret = -1;
		break;
	default:        /* devices, fifos, sockets */
		ret = mknodat(ddir, dname, st.st_mode, st.st_rdev);
		if (ret == 0 &&
		    (fchownat(ddir, dname, st.st_uid, st.st_gid, AT_SYMLINK_NOFOLLOW) < 0 ||
		     fchmodat(ddir, dname, st.st_mode & 07777, 0) < 0 ||
		     utimensat(ddir, dname, times, AT_SYMLINK_NOFOLLOW) < 0))
			ret = -1;
		break;
	}
	if (ret < 0)
		warn("cannot copy %s: %s", sname, strerror(errno));
	return ret;
}

int restore_path(int id, const char *arg)
{
	char path[PATH_MAX], parent[PATH_MAX], src[PATH_MAX], live[PATH_MAX];
	char tmp[64], comment[COMMENT_MAX];
	const char *rel, *leaf;
	struct snapshot s;
	struct stat st;
	int scope, sroot, lroot, sdir, ldir, exists, before, ret;

	normalize(arg, path, sizeof(path));
	for (int i = 0; unsnapshotted[i]; i++)
		if (under(path, unsnapshotted[i]))
			die("%s is not part of any snapshot", path);
	if (under(path, "/home")) {
		scope = SCOPE_HOME;
		rel = path + strlen("/home");
	} else {
		scope = SCOPE_SYSTEM;
		rel = path;
	}
	if (!*rel || strcmp(rel, "/") == 0)
		die("use 'snap rewind --%s' to restore all of %s",
		    scope_name(scope), scope_path(scope));

	if (store_get(id, &s) < 0)
		die("no snapshot %d", id);
	if (!(s.scope & scope))
		die("snapshot %d does not include %s", id, scope_path(scope));

	/* Split rel ("/a/b/c") into the parent ("a/b") and the leaf ("c"). */
	leaf = strrchr(rel, '/') + 1;
	if (leaf - rel > 1)
		snprintf(parent, sizeof(parent), "%.*s", (int)(leaf - rel - 2), rel + 1);
	else
		parent[0] = '\0';

	store_entry_path(src, sizeof(src), id, scope);
	path_join(live, sizeof(live), top, scope_subvol(scope));
	sroot = open(src, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	lroot = open(live, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (sroot < 0 || lroot < 0)
		die("cannot open the snapshot: %s", strerror(errno));

	sdir = open_in_root(sroot, parent);
	if (sdir < 0 || fstatat(sdir, leaf, &st, AT_SYMLINK_NOFOLLOW) < 0)
		die("%s does not exist in snapshot %d", path, id);
	ldir = open_in_root(lroot, parent);
	if (ldir < 0)
		die("the folder holding %s no longer exists; restore it first", path);
	exists = fstatat(ldir, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0;

	snprintf(comment, sizeof(comment), "Before restoring %s from %d", path, id);
	before = store_create(scope, "auto", comment);
	if (before < 0)
		die("cannot save the current state, nothing restored");

	snprintf(tmp, sizeof(tmp), ".snap-restore-%d", (int)getpid());
	remove_tree_at(ldir, tmp);
	if (copy_at(sdir, leaf, ldir, tmp) < 0) {
		remove_tree_at(ldir, tmp);
		die("restore failed, %s was not changed", path);
	}

	if (exists) {
		/* Swap atomically, then delete the old version. */
		ret = renameat2(ldir, tmp, ldir, leaf, RENAME_EXCHANGE);
		if (ret == 0)
			remove_tree_at(ldir, tmp);
	} else {
		ret = renameat(ldir, tmp, ldir, leaf);
	}
	if (ret < 0) {
		remove_tree_at(ldir, tmp);
		die("cannot put %s in place: %s", path, strerror(errno));
	}
	sync();

	printf("Restored %s from snapshot %d (previous state saved as snapshot %d)\n",
	       path, id, before);
	close(sdir);
	close(ldir);
	close(sroot);
	close(lroot);
	return 0;
}
