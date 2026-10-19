/* Btrfs subvolume operations, through the kernel ioctls. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/btrfs.h>

#include "snap.h"

/* Snapshot the subvolume at src as dstdir/name. */
int subvol_snapshot(const char *src, const char *dstdir, const char *name,
		    int readonly)
{
	struct btrfs_ioctl_vol_args_v2 args;
	int sfd, dfd, ret, err;

	sfd = open(src, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (sfd < 0)
		return -1;
	dfd = open(dstdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0) {
		err = errno;
		close(sfd);
		errno = err;
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.fd = sfd;
	args.flags = readonly ? BTRFS_SUBVOL_RDONLY : 0;
	snprintf(args.name, sizeof(args.name), "%s", name);
	ret = ioctl(dfd, BTRFS_IOC_SNAP_CREATE_V2, &args);

	err = errno;
	close(dfd);
	close(sfd);
	errno = err;
	return ret;
}

/* Delete the subvolume parent/name. */
int subvol_delete(const char *parent, const char *name)
{
	struct btrfs_ioctl_vol_args args;
	int fd, ret, err;

	fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	memset(&args, 0, sizeof(args));
	snprintf(args.name, sizeof(args.name), "%s", name);
	ret = ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &args);
	err = errno;
	close(fd);
	errno = err;
	return ret;
}
