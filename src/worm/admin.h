/* Checks shared by the setuid helpers (power, snapshots, install). */

#ifndef ADMIN_H
#define ADMIN_H

/* Is the caller (real user and groups) root or a member of wheel? */
int caller_is_admin(void);

/* Does password match root's? Waits after a wrong one. */
int root_password_ok(const char *password);

#endif
