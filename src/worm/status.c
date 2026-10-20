/*
 * worm status - the state of the system's protections at a glance.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "worm.h"

static void row(const char *name, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void row(const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("  %-11s ", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static int read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "re");
	int ok = f && fgets(buf, len, f);

	if (f)
		fclose(f);
	if (!ok)
		return -1;
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static int process_running(const char *name)
{
	struct dirent *e;
	DIR *d = opendir("/proc");
	int found = 0;

	while (!found && d && (e = readdir(d))) {
		char path[300], comm[64];

		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		found = read_line(path, comm, sizeof(comm)) == 0 && !strcmp(comm, name);
	}
	if (d)
		closedir(d);
	return found;
}

static void snapshots(void)
{
	static char out[65536];
	char newest[256] = "", automatic[64] = "";
	int count = 0;

	if (run((char *[]){ "snap", "list", NULL }, out, sizeof(out), 1) != 0) {
		row("Snapshots", "cannot list them");
		return;
	}
	for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
		char *p = line;

		while (*p == ' ')
			p++;
		if (!isdigit((unsigned char)*p))
			continue;
		count++;
		/* Squeeze the columns into one readable line. */
		newest[0] = '\0';
		for (size_t n = 0; *p && n < sizeof(newest) - 1; p++)
			if (*p != ' ' || (n && newest[n - 1] != ' '))
				newest[n++] = *p, newest[n] = '\0';
	}
	run((char *[]){ "snap", "auto", NULL }, automatic, sizeof(automatic), 1);
	if (!count)
		row("Snapshots", "none yet ('snap now' takes one)");
	else
		row("Snapshots", "%d, newest: %s", count, newest);
	row("", "automatic snapshots %s", strstr(automatic, "on") ? "on" : "OFF");
}

int cmd_status(void)
{
	char buf[256], *p;
	struct statvfs st;
	int total, running;

	printf("WormOS status\n");
	if (statvfs("/", &st) == 0)
		row("System", "%s", (st.f_flag & ST_RDONLY) ? "read-only" :
		    "UNLOCKED: writable until 'worm lock'");
	row("Mode", "%s", safe_mode() ? "SAFE MODE: no network" : "normal");
	snapshots();

	buf[0] = '\0';
	run((char *[]){ "snap", "stage", "status", NULL }, buf, sizeof(buf), 1);
	if (strstr(buf, "pending"))
		row("Packages", "a change is waiting for a restart");
	else if ((p = strstr(buf, "trial ")))
		row("Packages", "trial running (back to snapshot %d unless kept)", atoi(p + 6));
	else
		row("Packages", "no change waiting");

	row("Firewall", "%s", fw_summary());
	row("DNS", "%s", process_running("unbound") ? "encrypted (local resolver running)" :
	    "local resolver NOT RUNNING");
	running = blackholes_each(0, &total);
	row("Blackholes", "%d running, %d in total", running, total);

	if (read_line("/sys/kernel/security/lockdown", buf, sizeof(buf)) == 0 &&
	    (p = strchr(buf, '['))) {
		p[strcspn(p, "]")] = '\0';
		row("Kernel", "lockdown %s", p + 1);
	}
	if (read_line("/proc/sys/kernel/io_uring_disabled", buf, sizeof(buf)) == 0)
		row("", "io_uring %s", !strcmp(buf, "2") ? "off" : !strcmp(buf, "1") ?
		    "root only" : "ON");
	return 0;
}
