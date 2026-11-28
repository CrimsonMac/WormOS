/*
 * Timed snapshots and exports, one step at a time: the service
 * 12-snap-timer runs "snap timer-tick" every minute while /run/snap-status
 * says there is a schedule.
 *
 * Configuration (snap auto timer / snap auto export):
 *   timer = hourly|daily|off      a snapshot of the system and /home
 *   timer-keep = N                timed snapshots kept (older ones go;
 *                                 pinned ones stay)
 *   export-to = DIR               export the newest system snapshot there,
 *                                 whenever DIR is a mounted drive
 *   export-every = daily|weekly
 * The configuration also keeps "last-export = TIME ID" of the last one.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "snap.h"

#define EXPORTS_KEPT 3

static long interval(const char *word)
{
	if (!strcmp(word, "hourly"))
		return 3600;
	if (!strcmp(word, "daily"))
		return 86400;
	if (!strcmp(word, "weekly"))
		return 7 * 86400;
	return 0;
}

/* A folder on a drive of its own (not the system's disk). */
static int mounted_folder(const char *dir)
{
	char parent[PATH_MAX];
	struct stat a, b;

	snprintf(parent, sizeof(parent), "%s/..", dir);
	return stat(dir, &a) == 0 && S_ISDIR(a.st_mode) && stat(parent, &b) == 0 &&
	       a.st_dev != b.st_dev && access(dir, W_OK) == 0;
}

static void timed_snapshot(void)
{
	char timer[16] = "off", keep_s[16] = "";
	struct snapshot *list;
	long every;
	time_t last = 0, now = time(NULL);
	int n, keep, timed = 0, pid = 0, pscope;

	config_get("timer", timer, sizeof(timer));
	if (!(every = interval(timer)) || every == 7 * 86400)
		return;
	n = store_list(&list);
	for (int i = 0; i < n; i++)
		if (!strcmp(list[i].type, "timer") && list[i].date > last)
			last = list[i].date;
	free(list);
	/* A clock set back makes a snapshot "from the future": take one. */
	if (last && now >= last && now - last < every)
		return;
	if (store_create(SCOPE_SYSTEM | SCOPE_HOME, "timer",
			 every == 3600 ? "Hourly snapshot" : "Daily snapshot") < 0) {
		warn("the timed snapshot failed");
		return;
	}
	/* Only timed snapshots are pruned, never pinned ones. */
	keep = every == 3600 ? 24 : 14;
	if (config_get("timer-keep", keep_s, sizeof(keep_s)) && atoi(keep_s) > 0)
		keep = atoi(keep_s);
	if (pending_read(&pid, &pscope) <= 0)
		pid = 0;
	n = store_list(&list);
	for (int i = n - 1; i >= 0; i--)
		if (!strcmp(list[i].type, "timer") && !list[i].pinned && ++timed > keep &&
		    list[i].id != pid)
			store_delete(list[i].id);
	free(list);
}

static int cmp_export(const void *a, const void *b)
{
	int x = 0, y = 0;

	sscanf(strrchr(*(char *const *)a, '/') + 1, "wormos-snapshot-%d", &x);
	sscanf(strrchr(*(char *const *)b, '/') + 1, "wormos-snapshot-%d", &y);
	return x - y;
}

static void timed_export(void)
{
	char dir[PATH_MAX] = "", every_s[16] = "daily", last_s[64] = "", file[PATH_MAX + 64];
	struct snapshot *list;
	long long last_time = 0;
	int n, newest = 0, last_id = 0;
	glob_t g;

	if (!config_get("export-to", dir, sizeof(dir)) || !dir[0] || !mounted_folder(dir))
		return;
	config_get("export-every", every_s, sizeof(every_s));
	if (config_get("last-export", last_s, sizeof(last_s)))
		sscanf(last_s, "%lld %d", &last_time, &last_id);
	n = store_list(&list);
	for (int i = 0; i < n; i++)
		if (list[i].scope & SCOPE_SYSTEM)
			newest = list[i].id;
	free(list);
	if (!newest || newest == last_id ||
	    (time(NULL) >= last_time && time(NULL) - last_time < interval(every_s)))
		return;
	snprintf(file, sizeof(file), "%s/wormos-snapshot-%d.tar.zst", dir, newest);
	fprintf(stderr, "snap: exporting snapshot %d to %s\n", newest, dir);
	if (store_export(newest, file) < 0) {
		warn("exporting snapshot %d to %s failed", newest, dir);
		return;
	}
	snprintf(last_s, sizeof(last_s), "%lld %d", (long long)time(NULL), newest);
	config_set("last-export", last_s);
	/* The newest few stay on the drive. */
	snprintf(file, sizeof(file), "%s/wormos-snapshot-*.tar.zst", dir);
	if (glob(file, 0, NULL, &g) == 0) {
		qsort(g.gl_pathv, g.gl_pathc, sizeof(char *), cmp_export);
		for (size_t i = 0; i + EXPORTS_KEPT < g.gl_pathc; i++)
			unlink(g.gl_pathv[i]);
		globfree(&g);
	}
}

int timer_tick(void)
{
	timed_snapshot();
	timed_export();
	status_update();
	return 0;
}
