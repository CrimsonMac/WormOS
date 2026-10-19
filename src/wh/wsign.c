/*
 * wsign - create and check the Ed25519 signature of the package index.
 * A build host tool: the repository is signed where it is built.
 *
 *     wsign keygen SECRET PUBLIC   create a key pair
 *     wsign sign SECRET FILE       write FILE.sig
 *     wsign verify PUBLIC FILE     check FILE.sig
 *
 * Keys and signatures are stored as one line of hex.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <monocypher.h>
#include <monocypher-ed25519.h>

#include "sig.h"

static void die(const char *fmt, const char *arg)
{
	fprintf(stderr, "wsign: ");
	fprintf(stderr, fmt, arg, strerror(errno));
	fputc('\n', stderr);
	exit(1);
}

static void write_hex(const char *path, const uint8_t *data, size_t n, mode_t mode)
{
	char hex[2 * SIG_SECRET_LEN + 1];
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
	FILE *f;

	if (fd < 0 || !(f = fdopen(fd, "w")))
		die("cannot create %s: %s", path);
	hex_encode(data, n, hex);
	if (fprintf(f, "%s\n", hex) < 0 || fclose(f) != 0)
		die("cannot write %s: %s", path);
}

static int keygen(const char *secret, const char *public)
{
	uint8_t seed[32], sk[SIG_SECRET_LEN], pk[SIG_PUBLIC_LEN];

	if (getrandom(seed, sizeof(seed), 0) != sizeof(seed))
		die("cannot get random bytes%s: %s", "");
	crypto_ed25519_key_pair(sk, pk, seed);     /* wipes the seed */
	write_hex(secret, sk, sizeof(sk), 0600);
	write_hex(public, pk, sizeof(pk), 0644);
	crypto_wipe(sk, sizeof(sk));
	return 0;
}

static int sign(const char *secret, const char *file)
{
	uint8_t sk[SIG_SECRET_LEN], sig[SIG_LEN], *data;
	char sigfile[4096];
	size_t len;

	if (read_hex_file(secret, sk, sizeof(sk)) < 0)
		die("cannot read key %s: %s", secret);
	if (!(data = read_all(file, &len)))
		die("cannot read %s: %s", file);
	crypto_ed25519_sign(sig, sk, data, len);
	crypto_wipe(sk, sizeof(sk));
	free(data);

	snprintf(sigfile, sizeof(sigfile), "%s.sig", file);
	unlink(sigfile);
	write_hex(sigfile, sig, sizeof(sig), 0644);
	return 0;
}

static int verify(const char *public, const char *file)
{
	uint8_t pk[SIG_PUBLIC_LEN];
	char sigfile[4096];

	if (read_hex_file(public, pk, sizeof(pk)) < 0)
		die("cannot read key %s: %s", public);
	snprintf(sigfile, sizeof(sigfile), "%s.sig", file);
	if (sig_verify_file(pk, file, sigfile) < 0) {
		fprintf(stderr, "wsign: %s: BAD signature\n", file);
		return 1;
	}
	printf("%s: signature ok\n", file);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && strcmp(argv[1], "keygen") == 0)
		return keygen(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "sign") == 0)
		return sign(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "verify") == 0)
		return verify(argv[2], argv[3]);
	fprintf(stderr, "usage: wsign keygen SECRET PUBLIC | sign SECRET FILE | verify PUBLIC FILE\n");
	return 2;
}
