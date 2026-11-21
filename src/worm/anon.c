/*
 * worm anon - anonymous mode: all traffic of the machine through Tor.
 *
 *     worm anon on | off      switch it (kept across restarts)
 *     worm anon status        is it on, how far Tor has connected (for
 *                             everyone; users in the group tor also see
 *                             whether Tor runs and how far it got)
 *     worm anon start         start Tor if anonymous mode is on (at boot)
 *
 * The switch is the file /etc/worm/anon, so it is part of system snapshots
 * like the firewall's ports. The firewall (fw.c) then lets only Tor's own
 * user reach the network, sends every other TCP connection to Tor's
 * TransPort and every DNS query to its DNSPort, and rejects the rest (UDP,
 * ICMP, IPv6). The rules load before the network comes up and before Tor
 * starts, so nothing leaks while Tor connects; until then connections fail.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "worm.h"

#define ANON_CONFIG  "/etc/worm/anon"
#define TOR          "/usr/bin/tor"
#define TOR_RUN      "/run/tor"
#define TOR_PID      TOR_RUN "/tor.pid"
#define TOR_LOG      TOR_RUN "/notices.log"
#define TOR_DATA     "/var/lib/tor"

int anon_enabled(void)
{
	return access(ANON_CONFIG, F_OK) == 0;
}

long anon_tor_uid(void)
{
	struct passwd *pw = getpwnam("tor");

	return pw ? (long)pw->pw_uid : -1;
}

/* The running Tor's PID, or 0. */
static pid_t tor_pid(void)
{
	char path[64], comm[32] = "";
	long pid = 0;
	FILE *f;

	if ((f = fopen(TOR_PID, "re"))) {
		if (fscanf(f, "%ld", &pid) != 1)
			pid = 0;
		fclose(f);
	}
	if (pid <= 1)
		return 0;
	snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
	if (!(f = fopen(path, "re"))) {
		/*
		 * A user cannot see Tor's process (/proc has hidepid), but a
		 * signal 0 tells whether it exists. Users reach the pid file
		 * only through the tor group (WormOS Amnesia's live user).
		 */
		if (geteuid() != 0 && kill((pid_t)pid, 0) < 0 && errno == EPERM)
			return (pid_t)pid;
		return 0;
	}
	if (fgets(comm, sizeof(comm), f))
		comm[strcspn(comm, "\n")] = '\0';
	fclose(f);
	return strcmp(comm, "tor") == 0 ? (pid_t)pid : 0;
}

/* How far Tor has connected, from its log: 0 to 100, or -1. */
static int bootstrapped(void)
{
	char line[512], *p;
	int percent = -1;
	FILE *f = fopen(TOR_LOG, "re");

	while (f && fgets(line, sizeof(line), f))
		if ((p = strstr(line, "Bootstrapped ")))
			percent = atoi(p + 13);
	if (f)
		fclose(f);
	return percent;
}

static int tor_start(void)
{
	long uid = anon_tor_uid();

	if (tor_pid())
		return 0;
	if (uid < 0 || access(TOR, X_OK) < 0) {
		warn("Tor is not installed (wh install tor)%s", "");
		return -1;
	}
	/* Tor insists on owning its directories. */
	if ((mkdir(TOR_DATA, 0700) < 0 && errno != EEXIST) ||
	    chown(TOR_DATA, uid, uid) < 0 || chmod(TOR_DATA, 0700) < 0 ||
	    (mkdir(TOR_RUN, 0750) < 0 && errno != EEXIST) || chown(TOR_RUN, uid, uid) < 0) {
		warn("cannot prepare Tor's directories: %s", strerror(errno));
		return -1;
	}
	unlink(TOR_LOG);
	if (run((char *[]){ TOR, "-f", "/etc/tor/torrc", "--quiet", NULL }, NULL, 0, 0) != 0) {
		warn("Tor did not start (see %s)", TOR_LOG);
		return -1;
	}
	return 0;
}

static void tor_stop(void)
{
	pid_t pid = tor_pid();

	if (!pid)
		return;
	kill(pid, SIGTERM);
	for (int i = 0; i < 100 && tor_pid(); i++)
		usleep(50000);
	if (tor_pid())
		kill(pid, SIGKILL);
}

static void write_switch(int on)
{
	int relock = root_begin_change();
	int ok = 1;

	if (on) {
		FILE *f = fopen(ANON_CONFIG, "wxe");

		ok = f && fputs("# Anonymous mode is on (worm anon): all traffic goes through Tor.\n",
				f) >= 0;
		ok = f && fclose(f) == 0 && ok;
	} else {
		ok = unlink(ANON_CONFIG) == 0 || errno == ENOENT;
	}
	int err = errno;

	root_end_change(relock);
	if (!ok)
		die("cannot change " ANON_CONFIG ": %s", strerror(err));
}

static int anon_on(void)
{
	if (anon_tor_uid() < 0 || access(TOR, X_OK) < 0)
		die("Tor is not installed; 'wh install tor' first%s", "");
	if (!anon_enabled())
		write_switch(1);
	/* Rules first, then Tor: nothing gets out directly in between. */
	if (fw_reload() != 0)
		die("cannot load the firewall rules for anonymous mode%s", "");
	if (tor_start() < 0)
		return 1;
	printf("Anonymous mode is on: all traffic goes through Tor, and everything\n"
	       "else is blocked. Tor is connecting; 'worm anon status' shows how far.\n");
	return 0;
}

static int anon_off(void)
{
	if (anon_enabled())
		write_switch(0);
	if (fw_reload() != 0)
		die("cannot load the normal firewall rules%s", "");
	tor_stop();
	printf("Anonymous mode is off: traffic goes out directly again.\n");
	return 0;
}

const char *anon_summary(void)
{
	static char buf[96];
	int percent = bootstrapped();

	if (!anon_enabled())
		return "off";
	if (!tor_pid())
		return "on, but Tor is NOT RUNNING: no traffic gets out";
	if (percent >= 100)
		return "on: all traffic through Tor (connected)";
	snprintf(buf, sizeof(buf), "on: all traffic through Tor (connecting, %d%%)",
		 percent < 0 ? 0 : percent);
	return buf;
}

int cmd_anon(int argc, char **argv)
{
	if (argc != 1)
		return 2;
	if (!strcmp(argv[0], "on"))
		return anon_on();
	if (!strcmp(argv[0], "off"))
		return anon_off();
	if (!strcmp(argv[0], "status")) {
		printf("Anonymous mode: %s\n", anon_summary());
		return 0;
	}
	if (!strcmp(argv[0], "start"))
		return anon_enabled() && !safe_mode() ? (tor_start() < 0) : 0;
	return 2;
}
