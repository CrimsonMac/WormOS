/* Checking a user's password against /etc/shadow (needs root). */

#define _GNU_SOURCE
#include <crypt.h>
#include <shadow.h>
#include <string.h>
#include <unistd.h>

#include "password.h"

/* Compare without stopping at the first difference. */
static int same(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char diff = la != lb;

	for (size_t i = 0; i < la && i < lb; i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

int password_ok(const char *user, const char *password)
{
	struct crypt_data data;
	struct spwd *sp = getspnam(user);
	const char *hash;
	int ok = 0;

	memset(&data, 0, sizeof(data));
	if (sp) {
		if (sp->sp_pwdp[0] == '\0')
			ok = password[0] == '\0';
		else if (sp->sp_pwdp[0] != '!' && sp->sp_pwdp[0] != '*') {
			hash = crypt_r(password, sp->sp_pwdp, &data);
			ok = hash && hash[0] != '*' && same(hash, sp->sp_pwdp);
		}
	}
	explicit_bzero(&data, sizeof(data));
	if (!ok)
		sleep(2);
	return ok;
}
