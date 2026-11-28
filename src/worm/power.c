/*
 * power - shut down or restart the machine from the desktop, or panic.
 *
 * Installed setuid root as /usr/lib/worm/power for the shell's power menu.
 * It takes one word, "off", "reboot", "apply" or "panic", and nothing else from the
 * caller:
 * the environment is cleared, and the caller (by real user and groups)
 * must be root or a member of the group wheel. Then it signals worminit
 * (SIGUSR2 powers off, SIGTERM restarts), as busybox poweroff and reboot
 * do for root. "apply" finishes package changes that wait for a restart:
 * a quick restart (SIGHUP, the kernel keeps running) unless the kernel
 * changed, then a full one. "panic" runs 'worm panic' as root: it only protects (cuts
 * the network, stops blackholes, takes a snapshot), so it asks for no
 * password; bringing the network back ('worm panic off') needs root.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <grp.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "admin.h"

/* What wh's restart note asks for: 's'ystem, 'k'ernel, or 0 for none. */
static int restart_kind(void)
{
	char buf[16] = "";
	FILE *f = fopen("/run/wh/restart-needed", "re");

	if (!f)
		return 0;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);
	return buf[0];
}

int main(int argc, char **argv)
{
	int sig;

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	clearenv();
	if (argc != 2 || (strcmp(argv[1], "off") && strcmp(argv[1], "reboot") &&
			  strcmp(argv[1], "apply") && strcmp(argv[1], "panic"))) {
		fputs("usage: power off|reboot|apply|panic\n", stderr);
		return 2;
	}
	if (!caller_is_admin()) {
		fputs("power: only administrators (the group wheel) can do this\n", stderr);
		return 1;
	}
	if (!strcmp(argv[1], "panic")) {
		if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0) {
			perror("power");
			return 1;
		}
		setenv("PATH", "/usr/bin:/usr/sbin", 1);
		execl("/usr/bin/worm", "worm", "panic", (char *)NULL);
		perror("power: worm");
		return 1;
	}
	sig = strcmp(argv[1], "off") == 0 ? SIGUSR2 : SIGTERM;
	if (!strcmp(argv[1], "apply") && restart_kind() == 's')
		sig = SIGHUP;
	sync();
	if (kill(1, sig) < 0) {
		perror("power");
		return 1;
	}
	return 0;
}
