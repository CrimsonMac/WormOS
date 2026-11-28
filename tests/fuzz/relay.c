/*
 * bh's terminal output filter (src/bh/relay.c): whatever a program inside
 * a blackhole prints, in however many pieces, the host terminal must never
 * receive an operating system command (OSC) that osc_allowed() refuses.
 * The oracle reads the filtered output the way a terminal would: ESC ],
 * 0x9d or C2 9D, then the command's number.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "../../src/bh/relay.c"

static int out_fd = -1;

static void check(const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		size_t start;
		unsigned len = 0;

		if (p[i] == 0x1b && i + 1 < n && p[i + 1] == ']')
			start = i + 2;
		else if (p[i] == 0x9d)
			start = i + 1;
		else if (p[i] == 0xc2 && i + 1 < n && p[i + 1] == 0x9d)
			start = i + 2;
		else
			continue;
		while (start + len < n && p[start + len] >= '0' && p[start + len] <= '9')
			len++;
		/* More digits than the filter keeps are never allowed. */
		if (len && (len > 8 || !osc_allowed((const char *)p + start, len))) {
			fprintf(stderr, "refused OSC %.*s reached the terminal\n", (int)len,
				(const char *)p + start);
			abort();
		}
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct filter f = { 0 };
	static unsigned char back[1 << 20];
	size_t pos = 0;
	ssize_t got;

	if (out_fd < 0 && (out_fd = memfd_create("out", 0)) < 0)
		abort();
	if (ftruncate(out_fd, 0) < 0 || lseek(out_fd, 0, SEEK_SET) < 0)
		abort();
	/* The first byte of each piece says how long the piece is. */
	while (pos < size) {
		size_t piece = data[pos++] % 64 + 1;

		if (piece > size - pos)
			piece = size - pos;
		filter_write(out_fd, &f, (const char *)data + pos, piece);
		pos += piece;
	}
	if (lseek(out_fd, 0, SEEK_SET) < 0)
		abort();
	got = read(out_fd, back, sizeof(back));
	if (got > 0)
		check(back, (size_t)got);
	return 0;
}
