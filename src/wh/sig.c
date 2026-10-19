/* Ed25519 signatures for the package index, using Monocypher. */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocypher-ed25519.h>

#include "sig.h"

static int hex_value(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int hex_decode(const char *hex, uint8_t *out, size_t n)
{
	while (isspace((unsigned char)*hex))
		hex++;
	for (size_t i = 0; i < n; i++) {
		int hi = hex_value(hex[2 * i]), lo;

		if (hi < 0 || (lo = hex_value(hex[2 * i + 1])) < 0)
			return -1;
		out[i] = hi << 4 | lo;
	}
	hex += 2 * n;
	while (isspace((unsigned char)*hex))
		hex++;
	return *hex ? -1 : 0;
}

void hex_encode(const uint8_t *in, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", in[i]);
}

uint8_t *read_all(const char *path, size_t *len)
{
	FILE *f = fopen(path, "re");
	uint8_t *buf = NULL;
	size_t cap = 0, n = 0, r;

	if (!f)
		return NULL;
	do {
		if (n == cap) {
			uint8_t *p = realloc(buf, cap = cap ? cap * 2 : 65536);

			if (!p) {
				free(buf);
				fclose(f);
				errno = ENOMEM;
				return NULL;
			}
			buf = p;
		}
		r = fread(buf + n, 1, cap - n, f);
		n += r;
	} while (r > 0);
	if (ferror(f)) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = n;
	return buf;
}

int read_hex_file(const char *path, uint8_t *out, size_t n)
{
	char line[512];
	FILE *f = fopen(path, "re");
	int ok;

	if (!f)
		return -1;
	ok = fgets(line, sizeof(line), f) != NULL;
	fclose(f);
	if (!ok || hex_decode(line, out, n) < 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int sig_verify_file(const uint8_t key[SIG_PUBLIC_LEN], const char *file,
		    const char *sigfile)
{
	uint8_t sig[SIG_LEN], *data;
	size_t len;
	int ret;

	if (read_hex_file(sigfile, sig, sizeof(sig)) < 0)
		return -1;
	if (!(data = read_all(file, &len)))
		return -1;
	ret = crypto_ed25519_check(sig, key, data, len) == 0 ? 0 : -1;
	free(data);
	if (ret < 0)
		errno = EBADMSG;
	return ret;
}
