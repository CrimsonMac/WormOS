/*
 * Boot-time tasks, run by the initramfs before the system root is mounted:
 * clean up interrupted operations and apply a pending rewind.
 *
 * The target snapshot is verified against its checksums first. A rewind
 * then snapshots the current state, so it can itself be undone.
 * The live subvolume is then moved aside to <subvol>.old, replaced by a
 * writable snapshot of the target, and deleted.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snap.h"

static void delete_old(const char *subvol)
{
	char name[64], path[PATH_MAX];
	struct stat st;

	snprintf(name, sizeof(name), "%s.old", subvol);
	path_join(path, sizeof(path), top, name);
	if (stat(path, &st) == 0 && subvol_delete(top, name) < 0)
		warn("cannot delete %s: %s", name, strerror(errno));
}

static int replace_subvol(int id, int scope)
{
	char live[PATH_MAX], old[PATH_MAX], src[PATH_MAX], oldname[64];
	const char *subvol = scope_subvol(scope);

	snprintf(oldname, sizeof(oldname), "%s.old", subvol);
	path_join(live, sizeof(live), top, subvol);
	path_join(old, sizeof(old), top, oldname);
	store_entry_path(src, sizeof(src), id, scope);

	delete_old(subvol);
	if (rename(live, old) < 0) {
		warn("cannot move %s aside: %s", subvol, strerror(errno));
		return -1;
	}
	if (subvol_snapshot(src, top, subvol, 0) < 0) {
		warn("cannot restore %s from snapshot %d: %s",
		     scope_path(scope), id, strerror(errno));
		if (rename(old, live) < 0)
			warn("cannot put %s back: %s", subvol, strerror(errno));
		return -1;
	}
	delete_old(subvol);
	return 0;
}

static int apply_rewind(int id, int scope)
{
	struct snapshot s;
	char comment[64];
	int before, ret = 0;

	if (store_get(id, &s) < 0 || (s.scope & scope) != scope) {
		warn("pending rewind to snapshot %d (%s) is no longer possible",
		     id, scope_name(scope));
		return -1;
	}

	/* Never rewind into a snapshot that was tampered with. */
	if (store_verify(&s, scope) < 0) {
		warn("snapshot %d failed verification, rewind cancelled", id);
		return -1;
	}

	snprintf(comment, sizeof(comment), "Before rewind to %d", id);
	before = store_create(scope, "auto", comment);
	if (before < 0) {
		warn("cannot save the current state, rewind cancelled");
		return -1;
	}
	fprintf(stderr, "snap: saved the current %s as snapshot %d\n",
		scope_desc(scope), before);

	for (int bit = SCOPE_SYSTEM; bit <= SCOPE_HOME; bit <<= 1)
		if ((scope & bit) && replace_subvol(id, bit) < 0)
			ret = -1;
	sync();
	if (ret == 0)
		fprintf(stderr, "snap: rewound %s to snapshot %d\n",
			scope_desc(scope), id);
	return ret;
}

int boot_tasks(void)
{
	int id, scope, r;

	store_lock();
	store_cleanup();
	delete_old(SUBVOL_SYSTEM);
	delete_old(SUBVOL_HOME);

	r = pending_read(&id, &scope);
	if (r == 0)
		return 0;
	/* Clear the request first: a rewind that fails must not loop forever. */
	pending_clear();
	if (r < 0) {
		warn("ignoring a malformed rewind request");
		return 1;
	}
	return apply_rewind(id, scope) < 0 ? 1 : 0;
}
