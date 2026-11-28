/*
 * worm anon bridges auto (moat_parse in src/worm/anon.c): the answer of
 * the Tor Project's circumvention service, which arrives over the network.
 */

#define TOR_LOG "/nonexistent"

#include "common.h"
#include "../../src/worm/anon.c"

void die(const char *fmt, const char *arg) { (void)fmt; (void)arg; longjmp(fuzz_back, 1); }
void warn(const char *fmt, const char *arg) { (void)fmt; (void)arg; }
int run(char *const argv[], char *out, size_t len, int quiet) { (void)argv; (void)out; (void)len; (void)quiet; return -1; }
int safe_mode(void) { return 0; }
int fw_reload(void) { return 0; }
int root_begin_change(void) { return 0; }
void root_end_change(int relock) { (void)relock; }
int variant_is(const char *id) { (void)id; return 0; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FILE *out = fopen("/dev/null", "we");
	char *json = malloc(size + 1);
	int lines = 0;

	if (!out || !json)
		abort();
	memcpy(json, data, size);
	json[size] = '\0';
	if (!setjmp(fuzz_back))
		moat_parse(json, out, &lines);
	free(json);
	fclose(out);
	return 0;
}
