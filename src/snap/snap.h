/* Shared declarations for snap, the WormOS snapshot tool. */

#ifndef SNAP_H
#define SNAP_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/* Subvolumes on the top level of the btrfs filesystem. */
#define SUBVOL_SYSTEM    "@"
#define SUBVOL_HOME      "@home"
#define SUBVOL_STORE     "@snapshots"

/* Inside each snapshot directory @snapshots/<id>/. */
#define SNAP_SYSTEM      "system"
#define SNAP_HOME        "home"
#define SNAP_INFO        "info"

/* Files at the top of the store. */
#define STORE_LOCK       ".lock"
#define STORE_CONFIG     "config"
#define STORE_PENDING    "pending"

#define SCOPE_SYSTEM     1
#define SCOPE_HOME       2
#define SCOPE_ALL        (SCOPE_SYSTEM | SCOPE_HOME)

#define COMMENT_MAX      256

struct snapshot {
	int id;
	time_t date;
	int scope;                      /* SCOPE_* bits present */
	char type[16];                  /* manual, auto */
	int pinned;
	char comment[COMMENT_MAX];
};

/* Path of the mounted top-level subvolume. */
extern const char *top;

/* util.c */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void path_join(char *out, size_t len, const char *a, const char *b);
const char *scope_name(int scope);
const char *scope_desc(int scope);
const char *scope_path(int scope);   /* "/" or "/home" */
const char *scope_subvol(int scope); /* @ or @home */
const char *scope_dir(int scope);    /* system or home */
int remove_tree_at(int dfd, const char *name);

/* btrfs.c */
int subvol_snapshot(const char *src, const char *dstdir, const char *name,
		    int readonly);
int subvol_delete(const char *parent, const char *name);

/* store.c */
void store_lock(void);
int store_list(struct snapshot **out);    /* sorted by id, caller frees */
int store_get(int id, struct snapshot *s);
int store_create(int scope, const char *type, const char *comment);
int store_delete(int id);
void store_cleanup(void);
int store_write_info(const struct snapshot *s);
void store_entry_path(char *out, size_t len, int id, int scope);
int config_auto(void);
void config_set_auto(int on);
int pending_write(int id, int scope);
void pending_clear(void);
int pending_read(int *id, int *scope);

/* diff.c */
int diff_trees(const char *a, const char *b, const char *prefix);

/* restore.c */
int restore_path(int id, const char *path);

/* boot.c */
int boot_tasks(void);

#endif
