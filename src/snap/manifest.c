/*
 * Snapshot manifests: one line per file, recording everything that matters
 * about it, so tampering with a snapshot can be detected.
 *
 *     PATH TYPE MODE UID GID SIZE INODE MTIME CTIME DATA XATTRS
 *
 * TYPE is f, d, l, c, b, p or s. DATA is the SHA-256 of a file's contents
 * or a symlink's target, or MAJOR:MINOR for a device. XATTRS is the SHA-256
 * of the extended attributes (file capabilities, for example). Spaces,
 * backslashes and control characters in PATH are written as \ooo octal.
 *
 * The SHA-256 of the whole manifest is stored in the snapshot's info file.
 * Verifying a snapshot rebuilds the manifest from its contents and compares.
 *
 * Hashing every file of every snapshot would be slow, so a new manifest
 * reuses the hash from the previous snapshot's manifest for a file whose
 * inode, size and times are unchanged: btrfs snapshots keep all of these,
 * and the kernel updates the change time on any write.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "sha256.h"
#include "snap.h"

#define LINE_MAX_LEN  (4 * PATH_MAX + 512)
#define TABLE_SIZE    (1 << 16)

/* A manifest loaded into memory, indexed by path. */
struct entry {
	struct entry *next;
	char *line;
	size_t keylen;          /* length of the PATH field */
	int seen;
};

struct table {
	struct entry *bucket[TABLE_SIZE];
	int count;
};

static unsigned hash_key(const char *s, size_t n)
{
	unsigned h = 2166136261u;

	while (n--)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h & (TABLE_SIZE - 1);
}

static struct entry *table_find(struct table *t, const char *key, size_t n)
{
	for (struct entry *e = t->bucket[hash_key(key, n)]; e; e = e->next)
		if (e->keylen == n && memcmp(e->line, key, n) == 0)
			return e;
	return NULL;
}

static struct table *table_load(const char *path, unsigned char sum[SHA256_LEN])
{
	char line[LINE_MAX_LEN];
	struct sha256 ctx;
	struct table *t;
	FILE *f = fopen(path, "re");

	if (!f)
		return NULL;
	if (!(t = calloc(1, sizeof(*t))))
		die("out of memory");
	sha256_init(&ctx);
	while (fgets(line, sizeof(line), f)) {
		struct entry *e = malloc(sizeof(*e));
		size_t n;

		sha256_update(&ctx, line, strlen(line));
		line[strcspn(line, "\n")] = '\0';
		if (!e || !(e->line = strdup(line)))
			die("out of memory");
		n = strcspn(line, " ");
		e->keylen = n;
		e->seen = 0;
		e->next = t->bucket[hash_key(line, n)];
		t->bucket[hash_key(line, n)] = e;
		t->count++;
	}
	fclose(f);
	if (sum)
		sha256_final(&ctx, sum);
	return t;
}

static void table_free(struct table *t)
{
	if (!t)
		return;
	for (int i = 0; i < TABLE_SIZE; i++)
		for (struct entry *e = t->bucket[i], *next; e; e = next) {
			next = e->next;
			free(e->line);
			free(e);
		}
	free(t);
}

/* Field n (0-based) of a manifest line, copied into out. */
static void field(const char *line, int n, char *out, size_t len)
{
	while (n-- > 0 && (line = strchr(line, ' ')))
		line++;
	if (!line) {
		out[0] = '\0';
		return;
	}
	snprintf(out, len, "%.*s", (int)strcspn(line, " "), line);
}

static void escape(const char *in, char *out, size_t len)
{
	size_t n = 0;

	for (; *in && n + 5 < len; in++) {
		unsigned char c = *in;

		if (c <= ' ' || c == '\\' || c == 0x7f)
			n += snprintf(out + n, len - n, "\\%03o", c);
		else
			out[n++] = c;
	}
	out[n] = '\0';
}

static int hash_fd(int fd, char hex[SHA256_HEX])
{
	static unsigned char buf[1 << 16];
	unsigned char sum[SHA256_LEN];
	struct sha256 ctx;
	ssize_t n;

	sha256_init(&ctx);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		sha256_update(&ctx, buf, n);
	if (n < 0)
		return -1;
	sha256_final(&ctx, sum);
	sha256_hex(sum, hex);
	return 0;
}

static void hash_bytes(const void *data, size_t n, char hex[SHA256_HEX])
{
	unsigned char sum[SHA256_LEN];
	struct sha256 ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, data, n);
	sha256_final(&ctx, sum);
	sha256_hex(sum, hex);
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Hash the extended attributes of name (relative to dfd), "-" if none. */
static void hash_xattrs(int dfd, const char *name, char hex[SHA256_HEX])
{
	char path[PATH_MAX + 64], names[8192], *list[256];
	static char value[65536];
	unsigned char sum[SHA256_LEN];
	struct sha256 ctx;
	ssize_t n;
	int count = 0;

	/* There is no llistxattrat(), so go through /proc/self/fd. */
	snprintf(path, sizeof(path), "/proc/self/fd/%d/%s", dfd, name);
	n = llistxattr(path, names, sizeof(names));
	if (n <= 0) {
		snprintf(hex, SHA256_HEX, "-");
		return;
	}
	for (ssize_t i = 0; i < n && count < 256; i += strlen(names + i) + 1)
		list[count++] = names + i;
	qsort(list, count, sizeof(char *), cmp_str);

	sha256_init(&ctx);
	for (int i = 0; i < count; i++) {
		ssize_t v = lgetxattr(path, list[i], value, sizeof(value));

		if (v < 0)
			v = 0;
		sha256_update(&ctx, list[i], strlen(list[i]) + 1);
		sha256_update(&ctx, &v, sizeof(v));
		sha256_update(&ctx, value, v);
	}
	sha256_final(&ctx, sum);
	sha256_hex(sum, hex);
}

struct walker {
	struct sha256 ctx;      /* hash of all lines */
	FILE *out;              /* where to write the manifest, if anywhere */
	struct table *reuse;    /* previous manifest, for unchanged files */
	struct table *expect;   /* stored manifest, when verifying */
	FILE *report;           /* where differences are listed */
	int problems;
	int errors;
};

/* Report a difference found while verifying. */
static void problem(struct walker *w, char mark, const char *line)
{
	char path[PATH_MAX * 4];

	field(line, 0, path, sizeof(path));
	fprintf(w->report, "    %c %s\n", mark, path);
	w->problems++;
}

static void emit(struct walker *w, const char *line)
{
	size_t n = strlen(line);

	sha256_update(&w->ctx, line, n);
	sha256_update(&w->ctx, "\n", 1);
	if (w->out)
		fprintf(w->out, "%s\n", line);
	if (w->expect) {
		struct entry *e = table_find(w->expect, line, strcspn(line, " "));

		if (!e)
			problem(w, '+', line);
		else if (e->seen++, strcmp(e->line, line) != 0)
			problem(w, '~', line);
	}
}

static void walk(struct walker *w, int dfd, char *rel, size_t len)
{
	struct names names;
	size_t base = strlen(rel);

	read_names(dfd, &names);
	for (int i = 0; i < names.n; i++) {
		const char *name = names.v[i];
		char line[LINE_MAX_LEN], epath[PATH_MAX * 4], data[SHA256_HEX + 32];
		char xattrs[SHA256_HEX], type;
		struct stat st;

		if (base + strlen(name) + 2 > len) {
			warn("path too long: %s/%s", rel, name);
			w->errors++;
			continue;
		}
		snprintf(rel + base, len - base, "/%s", name);
		if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
			warn("cannot stat %s: %s", rel, strerror(errno));
			w->errors++;
			continue;
		}

		escape(rel, epath, sizeof(epath));
		snprintf(data, sizeof(data), "-");
		switch (st.st_mode & S_IFMT) {
		case S_IFREG: type = 'f'; break;
		case S_IFDIR: type = 'd'; break;
		case S_IFLNK: type = 'l'; break;
		case S_IFCHR: type = 'c'; break;
		case S_IFBLK: type = 'b'; break;
		case S_IFIFO: type = 'p'; break;
		default:      type = 's'; break;
		}

		if (type == 'f') {
			char prefix[256];
			struct entry *e;
			int fd;

			/* Unchanged since the previous snapshot? Reuse its hash. */
			snprintf(prefix, sizeof(prefix), " f%04o %u %u %lld %llu %lld.%09ld %lld.%09ld ",
				 st.st_mode & 07777, st.st_uid, st.st_gid,
				 (long long)st.st_size, (unsigned long long)st.st_ino,
				 (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
				 (long long)st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
			e = w->reuse ? table_find(w->reuse, epath, strlen(epath)) : NULL;
			if (e && strncmp(e->line + e->keylen, prefix, strlen(prefix)) == 0) {
				field(e->line, 8, data, sizeof(data));
			} else {
				fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
				if (fd < 0 || hash_fd(fd, data) < 0) {
					warn("cannot read %s: %s", rel, strerror(errno));
					w->errors++;
					snprintf(data, sizeof(data), "unreadable");
				}
				if (fd >= 0)
					close(fd);
			}
		} else if (type == 'l') {
			char target[PATH_MAX];
			ssize_t n = readlinkat(dfd, name, target, sizeof(target));

			if (n < 0) {
				warn("cannot read link %s: %s", rel, strerror(errno));
				w->errors++;
				n = 0;
			}
			hash_bytes(target, n, data);
		} else if (type == 'c' || type == 'b') {
			snprintf(data, sizeof(data), "%u:%u",
				 major(st.st_rdev), minor(st.st_rdev));
		}
		hash_xattrs(dfd, name, xattrs);

		snprintf(line, sizeof(line),
			 "%s %c%04o %u %u %lld %llu %lld.%09ld %lld.%09ld %s %s",
			 epath, type, st.st_mode & 07777, st.st_uid, st.st_gid,
			 (long long)st.st_size, (unsigned long long)st.st_ino,
			 (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
			 (long long)st.st_ctim.tv_sec, st.st_ctim.tv_nsec,
			 data, xattrs);
		emit(w, line);

		if (type == 'd') {
			int sub = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

			if (sub < 0) {
				warn("cannot open %s: %s", rel, strerror(errno));
				w->errors++;
			} else {
				walk(w, sub, rel, len);
				close(sub);
			}
		}
		rel[base] = '\0';
	}
	free_names(&names);
}

static int walk_tree(struct walker *w, const char *root, char hex[SHA256_HEX])
{
	char rel[PATH_MAX] = "";
	unsigned char sum[SHA256_LEN];
	int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0) {
		warn("cannot open %s: %s", root, strerror(errno));
		return -1;
	}
	sha256_init(&w->ctx);
	walk(w, fd, rel, sizeof(rel));
	close(fd);
	sha256_final(&w->ctx, sum);
	sha256_hex(sum, hex);
	return 0;
}

/*
 * Write the manifest of the snapshot at root to out, reusing hashes from
 * the manifest prev (may be NULL). The manifest's SHA-256 goes to sum.
 */
int manifest_create(const char *root, const char *out, const char *prev,
		    char sum[SHA256_HEX])
{
	struct walker w = { 0 };
	char tmp[PATH_MAX];
	int ok;

	snprintf(tmp, sizeof(tmp), "%s.tmp", out);
	w.out = fopen(tmp, "we");
	if (!w.out) {
		warn("cannot create %s: %s", tmp, strerror(errno));
		return -1;
	}
	if (prev)
		w.reuse = table_load(prev, NULL);
	ok = walk_tree(&w, root, sum) == 0 && w.errors == 0;
	table_free(w.reuse);

	ok &= fflush(w.out) == 0 && fsync(fileno(w.out)) == 0;
	ok &= fclose(w.out) == 0;
	if (!ok || rename(tmp, out) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/*
 * Check the snapshot at root against the checksum recorded when it was
 * taken, listing what changed to report. Returns 0 if intact, -1 otherwise.
 */
int manifest_verify(const char *root, const char *manifest, const char *expected,
		    FILE *report)
{
	struct walker w = { .report = report };
	unsigned char stored[SHA256_LEN];
	char hex[SHA256_HEX], stored_hex[SHA256_HEX];
	int stored_ok = 0;

	w.expect = table_load(manifest, stored);
	if (w.expect) {
		sha256_hex(stored, stored_hex);
		stored_ok = strcmp(stored_hex, expected) == 0;
		if (!stored_ok) {
			/* Its differences would not mean anything. */
			table_free(w.expect);
			w.expect = NULL;
		}
	}
	if (walk_tree(&w, root, hex) < 0) {
		table_free(w.expect);
		return -1;
	}
	if (w.expect) {
		for (int i = 0; i < TABLE_SIZE; i++)
			for (struct entry *e = w.expect->bucket[i]; e; e = e->next)
				if (!e->seen)
					problem(&w, '-', e->line);
		table_free(w.expect);
	}

	if (strcmp(hex, expected) != 0)
		return -1;
	if (!stored_ok)
		fprintf(report, "    the manifest file is damaged, but the contents are intact\n");
	return 0;
}
