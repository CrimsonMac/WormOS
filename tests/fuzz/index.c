/*
 * wh's package index (index_load in src/wh/repo.c) and package metadata
 * (parse_pkginfo in src/wh/pkg.c). Both are signed or checked against a
 * signed checksum before use, but they are parsed by root, so any input
 * must be refused cleanly (die) or read without overrunning anything.
 */

#include "common.h"
#include "../../src/wh/wh.h"

const char *root = "/nonexistent";
int root_fd = -1;
const char *key_file;

void hook_touch(const char *path) { (void)path; }

/* wh's die() for invalid input: back to the target instead of exiting. */
void __wrap_die(const char *fmt, ...)
{
	(void)fmt;
	if (fuzz_armed)
		longjmp(fuzz_back, 1);
	abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static struct index idx;
	struct package p;
	char *text;

	idx.pkgs = NULL;
	fuzz_armed = 1;
	if (!setjmp(fuzz_back)) {
		if (index_load(&idx, fuzz_file(data, size)) == 0)
			index_find(&idx, "wh");
	}
	free(idx.pkgs);
	idx.pkgs = NULL;

	if (!(text = malloc(size + 1)))
		abort();
	memcpy(text, data, size);
	text[size] = '\0';
	if (!setjmp(fuzz_back))
		parse_pkginfo(text, &p);
	free(text);
	fuzz_armed = 0;
	return 0;
}
