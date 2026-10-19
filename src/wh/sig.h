/* Ed25519 signatures for the package index. */

#ifndef SIG_H
#define SIG_H

#include <stddef.h>
#include <stdint.h>

#define SIG_PUBLIC_LEN  32
#define SIG_SECRET_LEN  64
#define SIG_LEN         64

/* Decode exactly n bytes of hex (whitespace around it is ignored). */
int hex_decode(const char *hex, uint8_t *out, size_t n);
void hex_encode(const uint8_t *in, size_t n, char *out);

/* Read a key or signature file: one line of hex. */
int read_hex_file(const char *path, uint8_t *out, size_t n);

/* Check that sigfile holds a valid signature of file by key. */
int sig_verify_file(const uint8_t key[SIG_PUBLIC_LEN], const char *file,
		    const char *sigfile);

/* Read a whole file into memory (caller frees). */
uint8_t *read_all(const char *path, size_t *len);

#endif
