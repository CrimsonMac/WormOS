/* Shared declarations for wh, the WormOS package manager. */

#ifndef WH_H
#define WH_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* Paths, relative to the root being managed. */
#define WH_DB_DIR       "usr/lib/wh/db"
#define WH_KEY          "usr/share/wh/keys/core.pub"
#define WH_REPO_CONF    "etc/wh/repo"
#define WH_STATE_DIR    "var/lib/wh"
#define WH_CACHE_DIR    "var/cache/wh"

#define NAME_MAX_LEN    64
#define VERSION_MAX_LEN 64
#define DESC_MAX_LEN    256
#define DEPS_MAX_LEN    512

/* A package as listed in the index or recorded in the database. */
struct package {
	char name[NAME_MAX_LEN];
	char version[VERSION_MAX_LEN];  /* VERSION-RELEASE */
	char description[DESC_MAX_LEN];
	char depends[DEPS_MAX_LEN];     /* space separated */
	char file[NAME_MAX_LEN + VERSION_MAX_LEN + 16];
	char sha256[65];
	long long size;
	long long installed_size;
};

struct index {
	struct package *pkgs;
	int count;
	long long date;
};

/* Options shared by all modules. */
extern const char *root;        /* "/" or --root DIR */
extern int root_fd;             /* open directory of the root */
extern int verbose;

/* util.c */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void root_path(char *out, size_t len, const char *rel);
int mkdirs_at(int dfd, const char *rel, int mode);
int open_in_root(const char *rel, int flags);
int run(char *const argv[], char *out, size_t outlen);
char *trim(char *s);
int word_in(const char *list, const char *word);

/* repo.c */
int index_load(struct index *idx, const char *path);
void index_free(struct index *idx);
struct package *index_find(struct index *idx, const char *name);
int repo_update(const char *url);
int repo_load(struct index *idx);
int repo_fetch(const char *url, const struct package *p, char *out, size_t len);
const char *repo_url(const char *override);

/* db.c */
int db_installed(const char *name, struct package *p);
int db_list(struct package **out);
int db_write(const char *name, const char *pkginfo, FILE *files);
int db_dir_used(const char *path, const char *except);
int db_remove(const char *name);
char *db_owner(const char *path, const char *except);   /* caller frees */
int db_files(const char *name, char ***paths, int *count);
void db_free_files(char **paths, int count);

/* pkg.c */
int pkg_install(const char *wpkg, const char *expect_name);
int pkg_remove(const char *name);
int parse_pkginfo(const char *text, struct package *p);

#endif
