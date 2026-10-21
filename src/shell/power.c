/*
 * Logging out ends the compositor (and so the session). Shutting down and
 * restarting go through /usr/lib/worm/power, which is setuid root and only
 * acts for members of the group wheel.
 */

#include "shell.h"

#define POWER_HELPER "/usr/lib/worm/power"

void power_logout(void)
{
	GError *error = NULL;

	if (!g_spawn_command_line_async("labwc --exit", &error)) {
		g_warning("cannot log out: %s", error->message);
		g_error_free(error);
	}
}

gboolean power_request(const char *what)
{
	char *argv[] = { POWER_HELPER, (char *)what, NULL };
	GError *error = NULL;
	int status;

	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
			  NULL, NULL, &status, &error)) {
		g_warning("cannot run %s: %s", POWER_HELPER, error->message);
		g_error_free(error);
		return FALSE;
	}
	return g_spawn_check_wait_status(status, NULL);
}
