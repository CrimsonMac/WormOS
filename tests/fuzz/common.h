#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Shared by the fuzz targets: input as a file, and die() that returns. */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static jmp_buf fuzz_back;
static int fuzz_armed;

/* The input in a memory file; its path, valid until the next call. */
static const char *fuzz_file(const uint8_t *data, size_t size)
{
	static int fd = -1;
	static char path[64];

	if (fd < 0) {
		if ((fd = memfd_create("input", 0)) < 0)
			abort();
		snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	}
	if (ftruncate(fd, 0) < 0 || pwrite(fd, data, size, 0) != (ssize_t)size)
		abort();
	return path;
}
