#ifndef NS_H
#define NS_H

/*
 * Namespace helpers shared by whc (wormholes) and bh (blackholes). They
 * return -1 with errno set on failure; callers report the error.
 */

#include <sys/types.h>

/* Write data to an existing file such as /proc/PID/uid_map. */
int ns_write_file(const char *path, const char *data);

/*
 * Map one user and one group ID in the user namespace of pid (0 for the
 * calling process): inside_uid there is outside_uid here, and the same
 * for the group. setgroups is denied first, as it must be for an
 * unprivileged caller.
 */
int ns_map_ids(pid_t pid, uid_t inside_uid, uid_t outside_uid,
	       gid_t inside_gid, gid_t outside_gid);

/*
 * Bind src onto root/dst, creating dst (a directory or an empty file to
 * match src). With MS_RDONLY in flags the bind is made read-only; the
 * flags of the mount src lives on are kept, as a user namespace requires.
 */
int ns_bind(const char *root, const char *src, const char *dst, unsigned long flags);

/* Make root the root of this mount namespace and detach the old one. */
int ns_pivot(const char *root);

#endif
