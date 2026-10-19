/*
 * The snapshot store: @snapshots on the top level of the btrfs filesystem.
 *
 *     @snapshots/
 *         .lock          held while the store is modified
 *         last-id        highest ID handed out, so IDs are never reused
 *         config         settings (auto = on|off)
 *         pending        a rewind to apply at the next boot: "<id> <scope>"
 *         <id>/
 *             info       metadata, key = value
 *             system     read-only snapshot of @    (when in scope)
 *             home       read-only snapshot of @home (when in scope)
 *
 * A snapshot is built under .new-<id> and renamed into place once complete,
 * so an interrupted snapshot never shows up as a valid one.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snap.h"

#define STORE_LASTID  "last-id"

static void store_path(char *out, size_t len, const char *name)
{
	char store[PATH_MAX];

	path_join(store, sizeof(store), top, SUBVOL_STORE);
	path_join(out, len, store, name);
}

void store_entry_path(char *out, size_t len, int id, int scope)
{
	char name[32];

	snprintf(name, sizeof(name), "%d", id);
	store_path(out, len, name);
	if (scope) {
		char entry[PATH_MAX];

		snprintf(entry, sizeof(entry), "%s", out);
		path_join(out, len, entry, scope_dir(scope));
	}
}

void store_lock(void)
{
	char path[PATH_MAX];
	int fd;

	store_path(path, sizeof(path), STORE_LOCK);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		die("cannot open %s: %s", path, strerror(errno));
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		fprintf(stderr, "snap: waiting for another snap command...\n");
		if (flock(fd, LOCK_EX) < 0)
			die("cannot lock the snapshot store: %s", strerror(errno));
	}
	/* The descriptor stays open, and the lock held, until exit. */
}

/* Read the first line of a store file. Returns 0 if it does not exist. */
static int read_line(const char *name, char *buf, size_t len)
{
	char path[PATH_MAX];
	FILE *f;

	store_path(path, sizeof(path), name);
	f = fopen(path, "re");
	if (!f)
		return 0;
	if (!fgets(buf, len, f))
		buf[0] = '\0';
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return 1;
}

/* Replace a file atomically: write a temporary file, then rename it. */
static int write_file(const char *path, const char *data)
{
	char tmp[PATH_MAX];
	FILE *f;
	int ok;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "we");
	if (!f)
		return -1;
	ok = fputs(data, f) >= 0;
	ok &= fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok &= fclose(f) == 0;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int write_store_file(const char *name, const char *data)
{
	char path[PATH_MAX];

	store_path(path, sizeof(path), name);
	return write_file(path, data);
}

static int parse_info(const char *path, struct snapshot *s)
{
	char line[COMMENT_MAX + 64];
	FILE *f = fopen(path, "re");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *key = line, *val;

		if (!eq)
			continue;
		*eq = '\0';
		val = eq + 1;
		while (*val == ' ')
			val++;
		val[strcspn(val, "\n")] = '\0';
		key[strcspn(key, " \t")] = '\0';

		if (strcmp(key, "date") == 0)
			s->date = strtoll(val, NULL, 10);
		else if (strcmp(key, "type") == 0)
			snprintf(s->type, sizeof(s->type), "%s", val);
		else if (strcmp(key, "pinned") == 0)
			s->pinned = strcmp(val, "yes") == 0;
		else if (strcmp(key, "comment") == 0)
			snprintf(s->comment, sizeof(s->comment), "%s", val);
	}
	fclose(f);
	return 0;
}

static int parse_id(const char *name)
{
	char *end;
	long id;

	if (!isdigit((unsigned char)name[0]))
		return -1;
	id = strtol(name, &end, 10);
	if (*end || id <= 0 || id > INT_MAX)
		return -1;
	return (int)id;
}

int store_get(int id, struct snapshot *s)
{
	char dir[PATH_MAX], path[PATH_MAX];
	struct stat st;

	memset(s, 0, sizeof(*s));
	s->id = id;
	store_entry_path(dir, sizeof(dir), id, 0);
	path_join(path, sizeof(path), dir, SNAP_INFO);
	if (parse_info(path, s) < 0)
		return -1;

	store_entry_path(path, sizeof(path), id, SCOPE_SYSTEM);
	if (stat(path, &st) == 0)
		s->scope |= SCOPE_SYSTEM;
	store_entry_path(path, sizeof(path), id, SCOPE_HOME);
	if (stat(path, &st) == 0)
		s->scope |= SCOPE_HOME;
	return 0;
}

static int cmp_id(const void *a, const void *b)
{
	return ((const struct snapshot *)a)->id - ((const struct snapshot *)b)->id;
}

int store_list(struct snapshot **out)
{
	char path[PATH_MAX];
	struct snapshot *list = NULL;
	struct dirent *e;
	int n = 0, cap = 0;
	DIR *d;

	path_join(path, sizeof(path), top, SUBVOL_STORE);
	d = opendir(path);
	if (!d)
		die("cannot open the snapshot store: %s", strerror(errno));
	while ((e = readdir(d))) {
		int id = parse_id(e->d_name);

		if (id < 0)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 32;
			list = realloc(list, cap * sizeof(*list));
			if (!list)
				die("out of memory");
		}
		if (store_get(id, &list[n]) == 0)
			n++;
	}
	closedir(d);
	if (n)
		qsort(list, n, sizeof(*list), cmp_id);
	*out = list;
	return n;
}

static int write_info(const char *dir, const struct snapshot *s)
{
	char path[PATH_MAX], buf[COMMENT_MAX + 128];

	path_join(path, sizeof(path), dir, SNAP_INFO);
	snprintf(buf, sizeof(buf),
		 "date = %lld\ntype = %s\npinned = %s\ncomment = %s\n",
		 (long long)s->date, s->type, s->pinned ? "yes" : "no",
		 s->comment);
	return write_file(path, buf);
}

int store_write_info(const struct snapshot *s)
{
	char dir[PATH_MAX];

	store_entry_path(dir, sizeof(dir), s->id, 0);
	return write_info(dir, s);
}

static int next_id(void)
{
	char buf[32];
	struct snapshot *list;
	int last = 0, n;

	if (read_line(STORE_LASTID, buf, sizeof(buf)))
		last = atoi(buf);
	n = store_list(&list);
	if (n && list[n - 1].id > last)
		last = list[n - 1].id;
	free(list);
	return last + 1;
}

/* Take a snapshot of the subvolumes in scope. Returns the new ID or -1. */
int store_create(int scope, const char *type, const char *comment)
{
	char tmpname[32], tmpdir[PATH_MAX], final[PATH_MAX], buf[32];
	struct snapshot s;
	int id = next_id();

	snprintf(tmpname, sizeof(tmpname), ".new-%d", id);
	store_path(tmpdir, sizeof(tmpdir), tmpname);
	if (mkdir(tmpdir, 0700) < 0) {
		warn("cannot create %s: %s", tmpdir, strerror(errno));
		return -1;
	}

	for (int bit = SCOPE_SYSTEM; bit <= SCOPE_HOME; bit <<= 1) {
		char src[PATH_MAX];

		if (!(scope & bit))
			continue;
		path_join(src, sizeof(src), top, scope_subvol(bit));
		if (subvol_snapshot(src, tmpdir, scope_dir(bit), 1) < 0) {
			warn("cannot snapshot %s: %s", scope_path(bit), strerror(errno));
			goto fail;
		}
	}

	memset(&s, 0, sizeof(s));
	s.id = id;
	s.date = time(NULL);
	snprintf(s.type, sizeof(s.type), "%s", type);
	/* Comments are stored on one line. */
	for (size_t i = 0; comment && comment[i] && i < sizeof(s.comment) - 1; i++)
		s.comment[i] = iscntrl((unsigned char)comment[i]) ? ' ' : comment[i];

	if (write_info(tmpdir, &s) < 0) {
		warn("cannot write snapshot info: %s", strerror(errno));
		goto fail;
	}

	store_entry_path(final, sizeof(final), id, 0);
	if (rename(tmpdir, final) < 0) {
		warn("cannot finish snapshot %d: %s", id, strerror(errno));
		goto fail;
	}
	snprintf(buf, sizeof(buf), "%d\n", id);
	write_store_file(STORE_LASTID, buf);
	sync();
	return id;

fail:
	subvol_delete(tmpdir, SNAP_SYSTEM);
	subvol_delete(tmpdir, SNAP_HOME);
	{
		char store[PATH_MAX];
		int dfd;

		path_join(store, sizeof(store), top, SUBVOL_STORE);
		dfd = open(store, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dfd >= 0) {
			remove_tree_at(dfd, tmpname);
			close(dfd);
		}
	}
	return -1;
}

/* Delete a snapshot directory (complete or not). */
static int delete_dir(const char *dir)
{
	char path[PATH_MAX];
	struct stat st;

	for (int bit = SCOPE_SYSTEM; bit <= SCOPE_HOME; bit <<= 1) {
		path_join(path, sizeof(path), dir, scope_dir(bit));
		if (stat(path, &st) == 0 && subvol_delete(dir, scope_dir(bit)) < 0) {
			warn("cannot delete %s: %s", path, strerror(errno));
			return -1;
		}
	}
	path_join(path, sizeof(path), dir, SNAP_INFO);
	unlink(path);
	if (rmdir(dir) < 0) {
		warn("cannot remove %s: %s", dir, strerror(errno));
		return -1;
	}
	return 0;
}

int store_delete(int id)
{
	char dir[PATH_MAX];

	store_entry_path(dir, sizeof(dir), id, 0);
	return delete_dir(dir);
}

/* Remove snapshots that were interrupted half way. */
void store_cleanup(void)
{
	char path[PATH_MAX], dir[PATH_MAX];
	struct dirent *e;
	DIR *d;

	path_join(path, sizeof(path), top, SUBVOL_STORE);
	d = opendir(path);
	if (!d)
		return;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, ".new-", 5) != 0)
			continue;
		path_join(dir, sizeof(dir), path, e->d_name);
		delete_dir(dir);
	}
	closedir(d);
}

int config_auto(void)
{
	char buf[64];

	if (!read_line(STORE_CONFIG, buf, sizeof(buf)))
		return 1;               /* on by default */
	return strstr(buf, "off") == NULL;
}

void config_set_auto(int on)
{
	if (write_store_file(STORE_CONFIG, on ? "auto = on\n" : "auto = off\n") < 0)
		die("cannot write the configuration: %s", strerror(errno));
}

int pending_write(int id, int scope)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%d %s\n", id, scope_name(scope));
	return write_store_file(STORE_PENDING, buf);
}

int pending_read(int *id, int *scope)
{
	char buf[64], name[16];

	if (!read_line(STORE_PENDING, buf, sizeof(buf)))
		return 0;
	if (sscanf(buf, "%d %15s", id, name) != 2)
		return -1;
	if (strcmp(name, "system") == 0)
		*scope = SCOPE_SYSTEM;
	else if (strcmp(name, "home") == 0)
		*scope = SCOPE_HOME;
	else if (strcmp(name, "all") == 0)
		*scope = SCOPE_ALL;
	else
		return -1;
	return 1;
}

void pending_clear(void)
{
	char path[PATH_MAX];

	store_path(path, sizeof(path), STORE_PENDING);
	unlink(path);
}
