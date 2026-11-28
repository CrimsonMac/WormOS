/*
 * Staged system changes, used by the package manager.
 *
 * The running root is never modified. Instead:
 *
 *   snap stage begin     creates @.next, a writable copy of @;
 *                        wh mounts it and installs packages into it.
 *   snap stage commit    snapshots the current system (the "before" state,
 *                        printed so wh can offer undo) and records that
 *                        @.next should replace @.
 *   (restart)            the boot tasks swap @.next in place of @.
 *
 * Changes stack: a change begun while another waits for a restart starts
 * from the waiting copy (moved aside to @.queued while the new one is
 * made), so nothing waiting is lost. An aborted change puts the waiting
 * copy back; a committed one replaces it and keeps its "before" snapshot,
 * which is still the state before everything that waits.
 *
 * A commit made with --try is a trial: the boot after the one that
 * activated it rolls the system back to the "before" snapshot, unless
 * "snap stage keep" was run in between.
 *
 * Store files: "activate" holds "<normal|try> <before-id>" while a change
 * waits to be applied, "trial" holds "<before-id>" while a trial runs.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snap.h"

static void top_path(char *out, size_t len, const char *name)
{
	path_join(out, len, top, name);
}

static void store_file(char *out, size_t len, const char *name)
{
	char store[PATH_MAX];

	path_join(store, sizeof(store), top, SUBVOL_STORE);
	path_join(out, len, store, name);
}

static int exists(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

/* Read "<word> <number>" from a store file. Returns 1 if found. */
static int read_pair(const char *name, char *word, size_t wlen, int *num)
{
	char path[PATH_MAX], buf[64], w[16];
	FILE *f;
	int ok;

	store_file(path, sizeof(path), name);
	if (!(f = fopen(path, "re")))
		return 0;
	ok = fgets(buf, sizeof(buf), f) != NULL;
	fclose(f);
	if (!ok || sscanf(buf, "%15s %d", w, num) != 2)
		return -1;
	if (word)
		snprintf(word, wlen, "%s", w);
	return 1;
}

static int write_pair(const char *name, const char *word, int num)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	FILE *f;
	int ok;

	store_file(path, sizeof(path), name);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (!(f = fopen(tmp, "we")))
		return -1;
	ok = fprintf(f, "%s %d\n", word, num) > 0;
	ok &= fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok &= fclose(f) == 0;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void remove_store_file(const char *name)
{
	char path[PATH_MAX];

	store_file(path, sizeof(path), name);
	unlink(path);
}

int stage_pending(void)
{
	char path[PATH_MAX];

	store_file(path, sizeof(path), STORE_ACTIVATE);
	return exists(path);
}

int stage_pending_before(void)
{
	int id;

	return stage_pending() && read_pair(STORE_ACTIVATE, NULL, 0, &id) > 0 ? id : 0;
}

int stage_trial(void)
{
	int id;

	return read_pair(STORE_TRIAL, NULL, 0, &id) > 0 ? id : 0;
}

/* Drop a staged copy and any activation request for it. */
void stage_discard(void)
{
	char path[PATH_MAX];

	remove_store_file(STORE_ACTIVATE);
	top_path(path, sizeof(path), SUBVOL_NEXT);
	if (exists(path) && subvol_delete(top, SUBVOL_NEXT) < 0)
		warn("cannot delete %s: %s", SUBVOL_NEXT, strerror(errno));
}

/* A change begun on top of one that waits: is one being made? */
static int queued(void)
{
	char path[PATH_MAX];

	top_path(path, sizeof(path), SUBVOL_QUEUED);
	return exists(path);
}

/* Abort the change being made; a change that waited keeps waiting. */
void stage_abort(void)
{
	char path[PATH_MAX], next[PATH_MAX];

	if (!queued()) {
		stage_discard();
		return;
	}
	top_path(next, sizeof(next), SUBVOL_NEXT);
	if (exists(next) && subvol_delete(top, SUBVOL_NEXT) < 0)
		warn("cannot delete %s: %s", SUBVOL_NEXT, strerror(errno));
	top_path(path, sizeof(path), SUBVOL_QUEUED);
	if (rename(path, next) < 0)
		warn("cannot put the waiting change back: %s", strerror(errno));
	sync();
}

/* Drop every change that waits, and one being made. */
void stage_drop(void)
{
	char path[PATH_MAX];

	if (queued() && subvol_delete(top, SUBVOL_QUEUED) < 0) {
		top_path(path, sizeof(path), SUBVOL_QUEUED);
		warn("cannot delete %s: %s", path, strerror(errno));
	}
	stage_discard();
}

int stage_begin(void)
{
	char live[PATH_MAX], next[PATH_MAX], q[PATH_MAX];

	if (stage_trial())
		die("a trial is running; run 'wh keep' or restart to end it first");
	top_path(next, sizeof(next), SUBVOL_NEXT);
	top_path(q, sizeof(q), SUBVOL_QUEUED);
	/* Left over from a change cut off half way: the waiting copy wins. */
	if (queued())
		stage_abort();
	if (stage_pending() && exists(next)) {
		if (rename(next, q) < 0)
			die("cannot set the waiting change aside: %s", strerror(errno));
		if (subvol_snapshot(q, top, SUBVOL_NEXT, 0) < 0) {
			int e = errno;

			stage_abort();
			die("cannot create %s: %s", SUBVOL_NEXT, strerror(e));
		}
		sync();
		return 0;
	}
	stage_discard();
	top_path(live, sizeof(live), SUBVOL_SYSTEM);
	if (subvol_snapshot(live, top, SUBVOL_NEXT, 0) < 0)
		die("cannot create %s: %s", SUBVOL_NEXT, strerror(errno));
	sync();
	return 0;
}

/* Returns the ID of the snapshot taken before the change, or 0 if none. */
int stage_commit(int trial, const char *comment)
{
	char next[PATH_MAX], mode[16] = "normal";
	int before = 0;

	top_path(next, sizeof(next), SUBVOL_NEXT);
	if (!exists(next))
		die("nothing is staged");

	/* On top of a waiting change: it is replaced, its "before" stays. */
	if (queued()) {
		if (read_pair(STORE_ACTIVATE, mode, sizeof(mode), &before) <= 0)
			before = 0;
		/* A trial returns to the waiting state, not to the one before it. */
		if (trial) {
			snprintf(mode, sizeof(mode), "try");
			store_system_from = SUBVOL_QUEUED;
			before = store_create(SCOPE_SYSTEM, "auto", comment);
			store_system_from = NULL;
			if (before < 0)
				die("cannot save the current state, change not applied");
		}
		if (subvol_delete(top, SUBVOL_QUEUED) < 0)
			warn("cannot delete %s: %s", SUBVOL_QUEUED, strerror(errno));
		if (write_pair(STORE_ACTIVATE, mode, before) < 0)
			die("cannot record the change: %s", strerror(errno));
		sync();
		return before;
	}

	/* A trial needs a state to return to, whatever "snap auto" says. */
	if (trial || config_auto()) {
		before = store_create(SCOPE_SYSTEM, "auto", comment);
		if (before < 0)
			die("cannot save the current state, change not applied");
	}
	if (write_pair(STORE_ACTIVATE, trial ? "try" : "normal", before) < 0)
		die("cannot record the change: %s", strerror(errno));
	sync();
	return before;
}

int stage_keep(void)
{
	if (!stage_trial())
		return -1;
	remove_store_file(STORE_TRIAL);
	sync();
	return 0;
}

/* Boot: end a trial that was not kept by rolling back. */
int stage_boot_trial(void)
{
	int id = stage_trial();

	if (!id)
		return 0;
	remove_store_file(STORE_TRIAL);
	fprintf(stderr, "snap: the trial was not kept, going back to snapshot %d\n", id);
	return boot_rewind(id, SCOPE_SYSTEM);
}

/* Boot: put a staged copy in place of the system. */
int stage_boot_activate(void)
{
	char mode[16], live[PATH_MAX], old[PATH_MAX], next[PATH_MAX];
	int before, r;

	/* A change cut off half way leaves the waiting one aside. */
	if (queued())
		stage_abort();
	r = read_pair(STORE_ACTIVATE, mode, sizeof(mode), &before);
	if (r == 0)
		return 0;
	remove_store_file(STORE_ACTIVATE);
	top_path(next, sizeof(next), SUBVOL_NEXT);
	if (r < 0 || !exists(next)) {
		warn("ignoring an incomplete package change");
		stage_discard();
		return -1;
	}

	top_path(live, sizeof(live), SUBVOL_SYSTEM);
	top_path(old, sizeof(old), SUBVOL_SYSTEM ".old");
	subvol_delete_old(SUBVOL_SYSTEM);
	if (rename(live, old) < 0) {
		warn("cannot move %s aside: %s", SUBVOL_SYSTEM, strerror(errno));
		return -1;
	}
	if (rename(next, live) < 0) {
		warn("cannot activate the change: %s", strerror(errno));
		if (rename(old, live) < 0)
			warn("cannot put %s back: %s", SUBVOL_SYSTEM, strerror(errno));
		return -1;
	}
	subvol_delete_old(SUBVOL_SYSTEM);

	if (strcmp(mode, "try") == 0 && before > 0 &&
	    write_pair(STORE_TRIAL, "snapshot", before) < 0)
		warn("cannot record the trial: %s", strerror(errno));
	sync();
	fprintf(stderr, "snap: applied the package change%s\n",
		strcmp(mode, "try") == 0 ? " (trial)" : "");
	return 0;
}
