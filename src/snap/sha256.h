#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN  32
#define SHA256_HEX  (2 * SHA256_LEN + 1)

struct sha256 {
	uint32_t h[8];
	uint64_t len;
	unsigned char buf[64];
	size_t used;
};

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t n);
void sha256_final(struct sha256 *c, unsigned char out[SHA256_LEN]);
void sha256_hex(const unsigned char sum[SHA256_LEN], char out[SHA256_HEX]);

#endif
