#ifndef WORM_H
#define WORM_H

#include <stddef.h>

void die(const char *fmt, const char *arg) __attribute__((noreturn));

/*
 * Run a command and return its exit status (-1 if it could not run). With
 * out, its standard output is captured there; quiet discards its errors.
 */
int run(char *const argv[], char *out, size_t len, int quiet);

/*
 * Make / writable for a short change to system configuration. Returns
 * whether root_end_change must make it read-only again (it was locked).
 */
int root_begin_change(void);
void root_end_change(int relock);

int cmd_fw(int argc, char **argv);

#endif
