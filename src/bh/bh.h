#ifndef BH_H
#define BH_H

#include <limits.h>
#include <sys/types.h>

#define BH_DIR      "/var/lib/blackholes"
#define BH_CGROUP   "/sys/fs/cgroup/blackholes"
/* Outside IDs for root inside blackholes opened by root: BH_ID_BASE + n. */
#define BH_ID_BASE  0x40000000u
#define BH_ID_COUNT 65536u
#define NAME_MAX_BH 32

struct box {
	char name[NAME_MAX_BH + 1];
	char dir[256];          /* BH_DIR/UID/NAME */
	uid_t id;               /* the outside ID of root inside the box */
};

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Start the box and run argv (a shell when NULL); returns its exit code. */
int box_run(const struct box *b, char *const argv[]);

/* bh's side of the program's terminal or output (relay.c). */
void relay_terminal(int sock, pid_t child);
void relay_output(int out, int err);

/* No new privileges, Landlock confined to /, then the seccomp filter. */
int sandbox_apply(void);

/* In each program before exec: keep only the harmless capabilities. */
int sandbox_drop_caps(void);

#endif
