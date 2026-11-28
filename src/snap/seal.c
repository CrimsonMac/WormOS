/*
 * Sealed snapshots: a forward-secure chain of signatures.
 *
 * Each snapshot's identity (number, date, scope, type and the checksums of
 * its manifests, which cover every file) is signed when the snapshot is
 * made, with a key that is destroyed right after: the signature also
 * certifies the next key, and only that next key is kept. So whoever gets
 * root later holds only keys newer than every existing snapshot and cannot
 * change one (its files, its manifest or its checksums) without the chain
 * showing it. Rewriting the whole chain needs a new first key, whose
 * fingerprint (snap seal) then differs from the one the user noted.
 *
 * Store files, in @snapshots/seal (0700):
 *   anchor   the first public key (hex)
 *   chain    one line per signature:
 *            "INDEX ID DATE SCOPE TYPE SYSTEM-SUM HOME-SUM NEXT-KEY SIGNATURE"
 *            (an absent checksum is "-"; keys and signatures in hex)
 *   key      the current secret: "INDEX SEED" at a fixed length, kept
 *            without copy-on-write and overwritten in place, so the old
 *            seed's blocks are reused rather than left behind
 *
 * The signed message is "wormos-snap-seal-1 INDEX ID DATE SCOPE TYPE
 * SYSTEM-SUM HOME-SUM NEXT-KEY". An index may appear twice in a row (a seal
 * cut off between writing the chain and the key is signed again by the
 * same key); the later line wins.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include "snap.h"

#define SEAL_DIR    "seal"
#define KEY_LINE    (10 + 1 + 64 + 1)      /* "%010d %64s\n" */

struct sealed {
	int id;
	long long date;
	int scope;
	char type[16];
	char sum[2][SHA256_HEX];
};

/* The verified chain, read once per run. */
static struct {
	int loaded;
	int state;              /* SEAL_* below */
	int length;             /* lines verified */
	int broken_at;          /* first line that failed, -1: none */
	uint8_t anchor[32];
	uint8_t last_next[32];  /* the key the last good line certified */
	int next_index;         /* the index that key signs */
	struct sealed *v;
	int n;
} chain;

enum { SEAL_NONE, SEAL_OK, SEAL_BROKEN };

static void seal_path(char *out, size_t len, const char *name)
{
	char store[PATH_MAX], dir[PATH_MAX];

	path_join(store, sizeof(store), top, SUBVOL_STORE);
	path_join(dir, sizeof(dir), store, SEAL_DIR);
	if (name)
		path_join(out, len, dir, name);
	else
		snprintf(out, len, "%s", dir);
}

static void to_hex(char *out, const uint8_t *in, size_t n)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * n] = '\0';
}

static int from_hex(uint8_t *out, const char *in, size_t n)
{
	if (strlen(in) != 2 * n)
		return -1;
	for (size_t i = 0; i < n; i++) {
		unsigned v;

		if (sscanf(in + 2 * i, "%2x", &v) != 1)
			return -1;
		out[i] = (uint8_t)v;
	}
	return 0;
}

static int message(char *out, size_t len, int index, const struct sealed *s, const char *next)
{
	int n = snprintf(out, len, "wormos-snap-seal-1 %d %d %lld %d %s %s %s %s", index, s->id,
			 s->date, s->scope, s->type, s->sum[0][0] ? s->sum[0] : "-",
			 s->sum[1][0] ? s->sum[1] : "-", next);

	return n > 0 && (size_t)n < len ? n : -1;
}

static void chain_load(void)
{
	char path[PATH_MAX], line[1024], hex[65];
	uint8_t pk[32], prev[32];
	int expect = 0;
	FILE *f;

	if (chain.loaded)
		return;
	chain.loaded = 1;
	chain.broken_at = -1;
	seal_path(path, sizeof(path), "anchor");
	if (!(f = fopen(path, "re")))
		return;
	if (!fgets(line, sizeof(line), f) || sscanf(line, "%64s", hex) != 1 ||
	    from_hex(chain.anchor, hex, 32) < 0) {
		fclose(f);
		chain.state = SEAL_BROKEN;
		chain.broken_at = 0;
		return;
	}
	fclose(f);
	chain.state = SEAL_OK;
	memcpy(pk, chain.anchor, 32);
	memcpy(prev, chain.anchor, 32);
	memcpy(chain.last_next, chain.anchor, 32);

	seal_path(path, sizeof(path), "chain");
	if (!(f = fopen(path, "re")))
		return;
	for (int lineno = 0; fgets(line, sizeof(line), f); lineno++) {
		struct sealed s = { 0 };
		char sums[2][SHA256_HEX + 2], next[80], sig[160], msg[512];
		uint8_t next_pk[32], sig_bin[64];
		const uint8_t *key;
		int index;

		if (sscanf(line, "%d %d %lld %d %15s %66s %66s %79s %159s", &index, &s.id, &s.date,
			   &s.scope, s.type, sums[0], sums[1], next, sig) != 9 ||
		    from_hex(next_pk, next, 32) < 0 || from_hex(sig_bin, sig, 64) < 0) {
			chain.state = SEAL_BROKEN;
			chain.broken_at = lineno;
			break;
		}
		for (int i = 0; i < 2; i++)
			snprintf(s.sum[i], sizeof(s.sum[i]), "%s", strcmp(sums[i], "-") ? sums[i] : "");
		/* The expected index, or the same one again (signed once more). */
		if (index == expect)
			key = pk;
		else if (index == expect - 1)
			key = prev;
		else
			key = NULL;
		if (!key || message(msg, sizeof(msg), index, &s, next) < 0 ||
		    crypto_ed25519_check(sig_bin, key, (const uint8_t *)msg, strlen(msg)) != 0) {
			chain.state = SEAL_BROKEN;
			chain.broken_at = lineno;
			break;
		}
		if (index == expect) {
			memcpy(prev, pk, 32);
			expect++;
		}
		memcpy(pk, next_pk, 32);
		memcpy(chain.last_next, next_pk, 32);
		chain.next_index = expect;
		if (!(chain.v = realloc(chain.v, (chain.n + 1) * sizeof(*chain.v))))
			die("out of memory");
		/* A later line for the same snapshot replaces the earlier one. */
		{
			int i;

			for (i = 0; i < chain.n && chain.v[i].id != s.id; i++)
				;
			chain.v[i] = s;
			if (i == chain.n)
				chain.n++;
		}
		chain.length = lineno + 1;
	}
	fclose(f);
}

/* Write the key file in place: same length, no copy-on-write. */
static int key_write(int index, const uint8_t seed[32])
{
	char path[PATH_MAX], line[KEY_LINE + 1], hex[65];
	int fd, flags = 0, ok;

	seal_path(path, sizeof(path), "key");
	fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0)
			return -1;
		/* Only an empty file can be switched to no copy-on-write. */
		if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0) {
			flags |= FS_NOCOW_FL;
			ioctl(fd, FS_IOC_SETFLAGS, &flags);
		}
	}
	to_hex(hex, seed, 32);
	snprintf(line, sizeof(line), "%010d %s\n", index, hex);
	ok = pwrite(fd, line, KEY_LINE, 0) == KEY_LINE && fdatasync(fd) == 0;
	crypto_wipe(line, sizeof(line));
	crypto_wipe(hex, sizeof(hex));
	ok &= close(fd) == 0;
	return ok ? 0 : -1;
}

static int key_read(int *index, uint8_t seed[32])
{
	char path[PATH_MAX], line[KEY_LINE + 1] = "", hex[65];
	int fd, ok;

	seal_path(path, sizeof(path), "key");
	if ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0)
		return -1;
	ok = read(fd, line, KEY_LINE) == KEY_LINE;
	close(fd);
	ok = ok && sscanf(line, "%d %64s", index, hex) == 2 && from_hex(seed, hex, 32) == 0;
	crypto_wipe(line, sizeof(line));
	crypto_wipe(hex, sizeof(hex));
	return ok ? 0 : -1;
}

static int write_new(const char *name, const char *text)
{
	char path[PATH_MAX];
	int fd, ok;

	seal_path(path, sizeof(path), name);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
	ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text) && fsync(fd) == 0;
	ok &= close(fd) == 0;
	return ok ? 0 : -1;
}

/* Start a chain: a first key, whose public half is the anchor. */
static int chain_start(void)
{
	char dir[PATH_MAX], hex[65], text[80];
	uint8_t seed[32], sk[64], pk[32];

	seal_path(dir, sizeof(dir), NULL);
	if (mkdir(dir, 0700) < 0 && errno != EEXIST)
		return -1;
	if (getrandom(seed, sizeof(seed), 0) != sizeof(seed))
		return -1;
	if (key_write(0, seed) < 0)
		return -1;
	crypto_ed25519_key_pair(sk, pk, seed);      /* wipes the seed */
	crypto_wipe(sk, sizeof(sk));
	to_hex(hex, pk, 32);
	snprintf(text, sizeof(text), "%s\n", hex);
	if (write_new("anchor", text) < 0)
		return -1;
	chain.loaded = 0;
	free(chain.v);
	memset(&chain, 0, sizeof(chain));
	return 0;
}

int seal_snapshot(const struct snapshot *snap)
{
	char path[PATH_MAX], msg[512], next_hex[65], sig_hex[129], line[700];
	uint8_t seed[32], next_seed[32], sk[64], pk[32], next_sk[64], next_pk[32], sig[64];
	struct sealed s = { 0 };
	struct stat st;
	int index, fd, ok;

	seal_path(path, sizeof(path), "anchor");
	if (lstat(path, &st) < 0) {
		char chain_file[PATH_MAX];

		/* A chain without its anchor is not started again quietly. */
		seal_path(chain_file, sizeof(chain_file), "chain");
		if (lstat(chain_file, &st) == 0) {
			warn("the seal chain lost its first key: snapshot %d is not sealed", snap->id);
			return -1;
		}
		if (chain_start() < 0) {
			warn("cannot start the seal chain: %s", strerror(errno));
			return -1;
		}
	}
	if (key_read(&index, seed) < 0) {
		warn("the sealing key is missing: snapshot %d is not sealed", snap->id);
		return -1;
	}
	crypto_ed25519_key_pair(sk, pk, seed);
	if (getrandom(next_seed, sizeof(next_seed), 0) != sizeof(next_seed)) {
		crypto_wipe(sk, sizeof(sk));
		return -1;
	}
	memcpy(seed, next_seed, 32);
	crypto_ed25519_key_pair(next_sk, next_pk, seed);   /* wipes seed, not next_seed */
	crypto_wipe(next_sk, sizeof(next_sk));

	s.id = snap->id;
	s.date = (long long)snap->date;
	s.scope = snap->scope;
	snprintf(s.type, sizeof(s.type), "%s", snap->type[0] ? snap->type : "manual");
	memcpy(s.sum, snap->sum, sizeof(s.sum));
	to_hex(next_hex, next_pk, 32);
	ok = message(msg, sizeof(msg), index, &s, next_hex) > 0;
	if (ok)
		crypto_ed25519_sign(sig, sk, (const uint8_t *)msg, strlen(msg));
	crypto_wipe(sk, sizeof(sk));
	if (!ok) {
		crypto_wipe(next_seed, sizeof(next_seed));
		return -1;
	}
	to_hex(sig_hex, sig, 64);
	snprintf(line, sizeof(line), "%d %d %lld %d %s %s %s %s %s\n", index, s.id, s.date,
		 s.scope, s.type, s.sum[0][0] ? s.sum[0] : "-", s.sum[1][0] ? s.sum[1] : "-",
		 next_hex, sig_hex);

	seal_path(path, sizeof(path), "chain");
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
	ok = fd >= 0 && write(fd, line, strlen(line)) == (ssize_t)strlen(line) && fsync(fd) == 0;
	if (fd >= 0)
		ok &= close(fd) == 0;
	/* Then the old key goes: only the next one is kept. */
	ok = ok && key_write(index + 1, next_seed) == 0;
	crypto_wipe(next_seed, sizeof(next_seed));
	if (!ok) {
		warn("cannot seal snapshot %d: %s", snap->id, strerror(errno));
		return -1;
	}
	chain.loaded = 0;
	free(chain.v);
	memset(&chain, 0, sizeof(chain));
	return 0;
}

int seal_check(const struct snapshot *s, char *why, size_t len)
{
	chain_load();
	for (int i = 0; i < chain.n; i++) {
		const struct sealed *c = &chain.v[i];

		if (c->id != s->id)
			continue;
		if (c->date != (long long)s->date || c->scope != s->scope ||
		    strcmp(c->sum[0], s->sum[0]) || strcmp(c->sum[1], s->sum[1])) {
			snprintf(why, len, "its checksums or date differ from the sealed ones");
			return -1;
		}
		return 1;
	}
	if (chain.state == SEAL_BROKEN) {
		snprintf(why, len, "the seal chain is broken (line %d)", chain.broken_at + 1);
		return -1;
	}
	snprintf(why, len, "not sealed (made before sealing existed, or imported)");
	return 0;
}

/* The anchor's fingerprint: the first 16 bytes of its SHA-256. */
static void fingerprint(char *out, size_t len)
{
	uint8_t digest[32];
	size_t n = 0;
	struct sha256 c;


	sha256_init(&c);
	sha256_update(&c, chain.anchor, 32);
	sha256_final(&c, digest);
	for (int i = 0; i < 16 && n < len; i += 2)
		n += (size_t)snprintf(out + n, len - n, "%s%02X%02X", i ? "-" : "", digest[i],
				      digest[i + 1]);
}

int seal_status(void)
{
	char fp[64];
	uint8_t seed[32], sk[64], pk[32];
	int index, key_ok;

	chain_load();
	if (chain.state == SEAL_NONE && !chain.n) {
		char path[PATH_MAX];
		struct stat st;

		seal_path(path, sizeof(path), "chain");
		if (lstat(path, &st) == 0) {
			printf("Seal: BROKEN, the chain's first key is missing\n");
			return 1;
		}
		printf("Seal: not started yet (the next snapshot starts it)\n");
		return 0;
	}
	fingerprint(fp, sizeof(fp));
	printf("First key fingerprint: %s\n", fp);
	printf("Sealed snapshots: %d (%d signatures)\n", chain.n, chain.length);
	key_ok = key_read(&index, seed) == 0;
	if (key_ok) {
		crypto_ed25519_key_pair(sk, pk, seed);
		crypto_wipe(sk, sizeof(sk));
		key_ok = index == chain.next_index && crypto_verify32(pk, chain.last_next) == 0;
	}
	if (chain.state == SEAL_BROKEN) {
		printf("Seal: BROKEN at signature %d: snapshots sealed after it cannot be trusted\n",
		       chain.broken_at + 1);
		return 1;
	}
	if (!key_ok) {
		printf("Seal: the current key does not continue the chain: new snapshots are "
		       "not sealed\n");
		return 1;
	}
	printf("Seal: intact. Compare the fingerprint with the one you noted: an attacker\n"
	       "who rewrote every seal would have had to start a new chain, with another\n"
	       "fingerprint.\n");
	return 0;
}
