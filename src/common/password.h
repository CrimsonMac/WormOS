/* Checking a user's password against /etc/shadow (needs root). */

#ifndef PASSWORD_H
#define PASSWORD_H

/*
 * Does password match the user's? An empty field (development images)
 * takes an empty password; a locked one ("!", "*") or a missing user
 * takes none. Waits two seconds after a wrong one.
 */
int password_ok(const char *user, const char *password);

#endif
