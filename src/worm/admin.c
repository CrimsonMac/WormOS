/* Checks shared by the setuid helpers (power, snapshots). */

#define _GNU_SOURCE
#include <crypt.h>
#include <grp.h>
#include <shadow.h>
#include <string.h>
#include <unistd.h>

#include "admin.h"

int caller_is_admin(void)
{
	struct group *wheel;
	gid_t groups[256];
	int n;

	if (getuid() == 0)
		return 1;
	if (!(wheel = getgrnam("wheel")))
		return 0;
	if (getgid() == wheel->gr_gid)
		return 1;
	n = getgroups(sizeof(groups) / sizeof(groups[0]), groups);
	for (int i = 0; i < n; i++)
		if (groups[i] == wheel->gr_gid)
			return 1;
	return 0;
}

/* Compare without stopping at the first difference. */
static int same(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char diff = la != lb;

	for (size_t i = 0; i < la && i < lb; i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

int root_password_ok(const char *password)
{
	struct crypt_data data;
	struct spwd *sp = getspnam("root");
	const char *hash;
	int ok = 0;

	if (sp) {
		/* An empty field (development images) takes an empty password;
		 * a locked one ("!", "*") takes none. */
		if (sp->sp_pwdp[0] == '\0')
			ok = password[0] == '\0';
		else if (sp->sp_pwdp[0] != '!' && sp->sp_pwdp[0] != '*') {
			memset(&data, 0, sizeof(data));
			hash = crypt_r(password, sp->sp_pwdp, &data);
			ok = hash && hash[0] != '*' && same(hash, sp->sp_pwdp);
		}
	}
	explicit_bzero(&data, sizeof(data));
	if (!ok)
		sleep(2);
	return ok;
}
