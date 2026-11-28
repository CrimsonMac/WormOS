/*
 * snap's seal chain (src/snap/seal.c): the anchor and chain files are
 * root's, but a chain is read by every verify and rewind, so a damaged or
 * hostile one must be reported as broken, never overrun anything. The
 * first input line is the anchor, the rest the chain.
 */

#include "common.h"

#include <sys/stat.h>
#include "../../src/snap/seal.c"

const char *top;
int top_given = 1;

void __wrap_die(const char *fmt, ...)
{
	(void)fmt;
	if (fuzz_armed)
		longjmp(fuzz_back, 1);
	abort();
}

static void put(const char *path, const uint8_t *data, size_t size)
{
	FILE *f = fopen(path, "we");

	if (!f || (size && fwrite(data, 1, size, f) != size) || fclose(f) != 0)
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static char dir[] = "/tmp/fuzz-seal-XXXXXX";
	static int made;
	char path[PATH_MAX];
	const uint8_t *nl = memchr(data, '\n', size);
	size_t first = nl ? (size_t)(nl - data) + 1 : size;
	struct snapshot s = { .id = 1, .scope = SCOPE_SYSTEM, .date = 1 };
	char why[128];

	if (!made) {
		if (!mkdtemp(dir))
			abort();
		snprintf(path, sizeof(path), "%s/@snapshots", dir);
		mkdir(path, 0700);
		snprintf(path, sizeof(path), "%s/@snapshots/seal", dir);
		mkdir(path, 0700);
		top = dir;
		made = 1;
	}
	snprintf(path, sizeof(path), "%s/@snapshots/seal/anchor", dir);
	put(path, data, first);
	snprintf(path, sizeof(path), "%s/@snapshots/seal/chain", dir);
	put(path, data + first, size - first);

	free(chain.v);
	memset(&chain, 0, sizeof(chain));
	fuzz_armed = 1;
	if (!setjmp(fuzz_back))
		seal_check(&s, why, sizeof(why));
	fuzz_armed = 0;
	return 0;
}
