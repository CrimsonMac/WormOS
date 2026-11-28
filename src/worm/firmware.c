/*
 * worm firmware - firmware updates through Debian's fwupd (LVFS).
 *
 *     worm firmware [devices]    what fwupd finds and can update
 *     worm firmware check        download the firmware list, show updates
 *     worm firmware update       install them (asks first)
 *
 * fwupd is a large program with many libraries and an EFI binary of its
 * own, so instead of building it, WormOS runs Debian's (fwupd and, on
 * UEFI machines, fwupd-amd64-signed, whose EFI binary Debian signed and
 * the Debian shim WormOS boots through trusts) in the Debian wormhole, as
 * root, through its offline tool fwupdtool. For it, the EFI variables are
 * mounted (efivarfs), the EFI system partition is mounted at
 * /run/worm/esp and bound at /boot/efi inside, where fwupd.conf tells
 * fwupd to look, and udev's device database is bound in. The wormhole is
 * set up and fwupd installed the first time (that needs the network).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "worm.h"

#define WORMHOLE   "/var/lib/wormholes/debian"
#define ESP_DIR    "/run/worm/esp"
#define EFIVARS    "/sys/firmware/efi/efivars"

static int uefi(void)
{
	return access("/sys/firmware/efi", F_OK) == 0;
}

static int mounted(const char *dir)
{
	char line[1024];
	size_t n = strlen(dir);
	int found = 0;
	FILE *f = fopen("/proc/self/mounts", "re");

	while (f && !found && fgets(line, sizeof(line), f)) {
		char *p = strchr(line, ' ');

		found = p && !strncmp(p + 1, dir, n) && p[1 + n] == ' ';
	}
	if (f)
		fclose(f);
	return found;
}

/* The block device name (vda2, nvme0n1p2, dm-0) the root is on. */
static int root_device(char *out, size_t len)
{
	char link[PATH_MAX];
	ssize_t n = readlink("/dev/wormos-root", link, sizeof(link) - 1);

	if (n <= 0)
		return -1;
	link[n] = '\0';
	snprintf(out, len, "%s", strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
	return 0;
}

/*
 * The EFI system partition: partition 1 of the disk the root is on (the
 * layout of every WormOS disk), through the encrypted device's partition
 * when the root is encrypted.
 */
static int find_esp(char *out, size_t len)
{
	char dev[NAME_MAX + 1], path[PATH_MAX], link[PATH_MAX], disk[NAME_MAX + 1];
	struct dirent *e;
	ssize_t n;
	DIR *d;

	if (root_device(dev, sizeof(dev)) < 0)
		return -1;
	/* An encrypted root: the partition under it. */
	snprintf(path, sizeof(path), "/sys/block/%s/slaves", dev);
	if ((d = opendir(path))) {
		while ((e = readdir(d)))
			if (e->d_name[0] != '.')
				snprintf(dev, sizeof(dev), "%s", e->d_name);
		closedir(d);
	}
	/* The disk is the partition's parent in sysfs. */
	snprintf(path, sizeof(path), "/sys/class/block/%s", dev);
	if ((n = readlink(path, link, sizeof(link) - 1)) <= 0)
		return -1;
	link[n] = '\0';
	*strrchr(link, '/') = '\0';
	snprintf(disk, sizeof(disk), "%s", strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
	snprintf(path, sizeof(path), "/sys/block/%s", disk);
	if (!(d = opendir(path)))
		return -1;
	while ((e = readdir(d))) {
		char part[PATH_MAX + 300], num[16] = "";
		FILE *f;

		snprintf(part, sizeof(part), "%s/%s/partition", path, e->d_name);
		if (!(f = fopen(part, "re")))
			continue;
		if (fgets(num, sizeof(num), f) && atoi(num) == 1) {
			snprintf(out, len, "/dev/%s", e->d_name);
			fclose(f);
			closedir(d);
			return 0;
		}
		fclose(f);
	}
	closedir(d);
	return -1;
}

/* fwupd in the Debian wormhole, set up the first time. */
static void ensure_fwupd(void)
{
	char conf[] = WORMHOLE "/etc/fwupd/fwupd.conf", *text = NULL, line[256];
	size_t n = 0;
	int has = 0;
	FILE *f, *mem;

	if (access(WORMHOLE "/usr/bin/fwupdtool", X_OK) < 0) {
		printf("Installing fwupd from Debian (the first time only)...\n");
		fflush(stdout);
		if (run(uefi() ? (char *[]){ "wh", "-y", "install", "--from", "debian", "fwupd",
					     "fwupd-amd64-signed", NULL }
			       : (char *[]){ "wh", "-y", "install", "--from", "debian", "fwupd", NULL },
			NULL, 0, 0) != 0 || access(WORMHOLE "/usr/bin/fwupdtool", X_OK) < 0)
			die("cannot install fwupd from Debian%s", "");
	}
	/* Where fwupd finds the EFI system partition (no udisks here). */
	if (!(mem = open_memstream(&text, &n)))
		die("out of memory%s", "");
	if ((f = fopen(conf, "re"))) {
		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, "EspLocation=", 12))
				has = 1;
			fputs(line, mem);
			if (!strncmp(line, "[fwupd]", 7) && !has) {
				fputs("EspLocation=/boot/efi\n", mem);
				has = 1;
			}
		}
		fclose(f);
	}
	if (!has)
		fputs("[fwupd]\nEspLocation=/boot/efi\n", mem);
	fclose(mem);
	if ((f = fopen(conf, "we"))) {
		fputs(text, f);
		fclose(f);
	}
	free(text);
}

static int fwupdtool(char *const args[])
{
	char *argv[24];
	int n = 0, status;
	pid_t pid;

	argv[n++] = "whc";
	argv[n++] = "--root";
	argv[n++] = "--bind";
	argv[n++] = ESP_DIR ":/boot/efi";
	argv[n++] = "--bind";
	argv[n++] = "/run/udev:/run/udev";
	argv[n++] = "debian";
	argv[n++] = "fwupdtool";
	for (int i = 0; args[i] && n < 22; i++)
		argv[n++] = args[i];
	argv[n] = NULL;
	if ((pid = fork()) < 0)
		return -1;
	if (pid == 0) {
		execvp("whc", argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int cmd_firmware(int argc, char **argv)
{
	const char *what = argc > 0 ? argv[0] : "devices";
	char esp[PATH_MAX];
	int esp_mounted = 0, ret;

	if (argc > 1 || (strcmp(what, "devices") && strcmp(what, "check") && strcmp(what, "update")))
		return 2;
	ensure_fwupd();
	if (uefi() && !mounted(EFIVARS) &&
	    mount("efivarfs", EFIVARS, "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
		warn("cannot mount the EFI variables: %s", strerror(errno));
	mkdir("/run/worm", 0711);
	mkdir(ESP_DIR, 0700);
	if (!mounted(ESP_DIR)) {
		if (find_esp(esp, sizeof(esp)) == 0 &&
		    mount(esp, ESP_DIR, "vfat", MS_NOSUID | MS_NODEV, "umask=0077") == 0)
			esp_mounted = 1;
		else if (uefi())
			warn("cannot find or mount the EFI system partition%s", "");
	}
	if (!strcmp(what, "devices")) {
		ret = fwupdtool((char *[]){ "get-devices", NULL });
	} else if (!strcmp(what, "check")) {
		fwupdtool((char *[]){ "refresh", "--force", NULL });
		ret = fwupdtool((char *[]){ "get-updates", NULL });
	} else {
		fwupdtool((char *[]){ "refresh", "--force", NULL });
		ret = fwupdtool((char *[]){ "update", NULL });
		if (ret == 0)
			printf("Some firmware is installed at the next restart (the computer "
			       "shows its progress).\n");
	}
	if (esp_mounted)
		umount(ESP_DIR);
	return ret == 0 ? 0 : 1;
}
