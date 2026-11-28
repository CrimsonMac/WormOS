/*
 * snap's manifests (manifest_verify in src/snap/manifest.c), read at every
 * verify and rewind and from imported archives: the parser must refuse
 * anything malformed. The checksum the manifest must match is computed
 * here, so the fuzzer reaches the line parser; the tree is an empty folder.
 */

#include "common.h"

#include <sys/stat.h>
#include "../../src/snap/snap.h"
#include "../../src/snap/sha256.h"

const char *top = "/nonexistent";
int top_given = 1;

void __wrap_die(const char *fmt, ...)
{
	(void)fmt;
	if (fuzz_armed)
		longjmp(fuzz_back, 1);
	abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static char dir[] = "/tmp/fuzz-manifest-XXXXXX";
	static int made;
	struct sha256 c;
	unsigned char sum[SHA256_LEN];
	char hex[SHA256_HEX];
	FILE *null = fopen("/dev/null", "we");

	if (!null)
		abort();
	if (!made) {
		if (!mkdtemp(dir))
			abort();
		made = 1;
	}
	sha256_init(&c);
	sha256_update(&c, data, size);
	sha256_final(&c, sum);
	sha256_hex(sum, hex);
	fuzz_armed = 1;
	if (!setjmp(fuzz_back))
		manifest_verify(dir, fuzz_file(data, size), hex, null);
	fuzz_armed = 0;
	fclose(null);
	return 0;
}
