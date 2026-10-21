/*
 * power - shut down or restart the machine from the desktop.
 *
 * Installed setuid root as /usr/lib/worm/power for the shell's power menu.
 * It takes one word, "off" or "reboot", and nothing else from the caller:
 * the environment is cleared, and the caller (by real user and groups)
 * must be root or a member of the group wheel. Then it signals worminit
 * (SIGUSR2 powers off, SIGTERM restarts), as busybox poweroff and reboot
 * do for root.
 */

#define _GNU_SOURCE
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int in_wheel(void)
{
	struct group *wheel = getgrnam("wheel");
	gid_t groups[256];
	int n;

	if (!wheel)
		return 0;
	if (getgid() == wheel->gr_gid)
		return 1;
	n = getgroups(sizeof(groups) / sizeof(groups[0]), groups);
	for (int i = 0; i < n; i++)
		if (groups[i] == wheel->gr_gid)
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	int sig;

	clearenv();
	if (argc != 2 || (strcmp(argv[1], "off") && strcmp(argv[1], "reboot"))) {
		fputs("usage: power off|reboot\n", stderr);
		return 2;
	}
	if (getuid() != 0 && !in_wheel()) {
		fputs("power: only administrators (the group wheel) can do this\n", stderr);
		return 1;
	}
	sig = strcmp(argv[1], "off") == 0 ? SIGUSR2 : SIGTERM;
	sync();
	if (kill(1, sig) < 0) {
		perror("power");
		return 1;
	}
	return 0;
}
