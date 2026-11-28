/*
 * A summary of the snapshots for everyone: the store itself is only
 * reachable by root (and only while snap runs), but the desktop and the
 * shell prompt show how many snapshots there are and when the last one was
 * taken. snap rewrites /run/snap-status whenever the list changes, and
 * "snap status" refreshes it at boot:
 *
 *     count = 12
 *     last = 14
 *     last_date = 1761052800
 *     last_comment = before the update
 *     auto = on
 *     pending = rewind
 *
 * pending is "rewind", "change" (a staged package change), "trial" or
 * empty.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snap.h"

#define STATUS_FILE "/run/snap-status"

static void write_status(FILE *f)
{
	struct snapshot *list;
	int n = store_list(&list), id, scope;
	const char *pending = "";

	if (pending_read(&id, &scope) > 0)
		pending = "rewind";
	else if (stage_trial())
		pending = "trial";
	else if (stage_pending())
		pending = "change";
	fprintf(f, "count = %d\n", n);
	if (n > 0) {
		char comment[COMMENT_MAX];

		/* One line per value. */
		snprintf(comment, sizeof(comment), "%s", list[n - 1].comment);
		comment[strcspn(comment, "\n")] = '\0';
		fprintf(f, "last = %d\nlast_date = %lld\nlast_comment = %s\n",
			list[n - 1].id, (long long)list[n - 1].date, comment);
	}
	fprintf(f, "auto = %s\npending = %s\n", config_auto() ? "on" : "off", pending);
	/* For the timer service: whether there is anything to do. */
	{
		char timer[16] = "off", dest[8] = "";

		config_get("timer", timer, sizeof(timer));
		config_get("export-to", dest, sizeof(dest));
		fprintf(f, "timer = %s\nschedule = %s\n", timer,
			strcmp(timer, "off") || dest[0] ? "on" : "off");
	}
	free(list);
}

void status_update(void)
{
	char tmp[] = STATUS_FILE ".XXXXXX";
	FILE *f;
	int fd;

	/* Only for the running system, not the initramfs or tests (--top). */
	if (top_given || geteuid() != 0)
		return;
	if ((fd = mkstemp(tmp)) < 0)
		return;
	if (fchmod(fd, 0644) < 0 || !(f = fdopen(fd, "w"))) {
		close(fd);
		unlink(tmp);
		return;
	}
	write_status(f);
	if (fclose(f) != 0 || rename(tmp, STATUS_FILE) < 0) {
		warn("cannot update %s: %s", STATUS_FILE, strerror(errno));
		unlink(tmp);
	}
}

void status_print(void)
{
	write_status(stdout);
}
