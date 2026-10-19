#!/bin/bash
# Assemble the root filesystem by installing packages from the repository.
#
# Runs under fakeroot so files can be owned by root without real privileges.
# The ownership database is saved to build/fakeroot.state for mkimage.sh.
#
# The result is one directory per btrfs subvolume under build/fsroot:
#   @           the system root, mounted read-only
#   @var        /var, writable state and logs (not part of system snapshots)
#   @home       /home
#   @snapshots  snapshot store, never mounted by the running system

. "$(dirname "$0")/env.sh"

STATE=$BUILD/fakeroot.state
if [ -z "${FAKEROOTKEY:-}" ]; then
	rm -f "$STATE"
	exec fakeroot -s "$STATE" -- "$0" "$@"
fi

# The base system. Packages come from the signed repository (tools/wrepo)
# and are installed with wh, as on a running system.
PACKAGES=(base-files glibc gcc-libs busybox worminit wormrd snap wh linux
	zstd curl)
WH=$BUILD/host/wh

[ -x "$WH" ] && [ -f "$REPO/index" ] || die "run tools/wrepo first"

msg "Assembling root filesystem"
rm -rf "$FSROOT"
mkdir -p "$ROOTFS" "$FSROOT/@home" "$FSROOT/@snapshots"

cd "$ROOTFS"
mkdir -p boot dev etc home mnt proc root run sys tmp \
	usr/bin usr/lib usr/sbin usr/share \
	var/cache var/lib var/log var/spool var/roothome
ln -s usr/bin bin
ln -s usr/sbin sbin
ln -s usr/lib lib
ln -s usr/lib lib64
ln -s lib usr/lib64
ln -s ../run var/run
ln -s ../run/lock var/lock
ln -s ../tmp var/tmp

wh() { "$WH" --root "$ROOTFS" --repo "file://$REPO/" --key "$WORMOS_ROOT/keys/core.pub" "$@"; }
wh update
wh install "${PACKAGES[@]}"
# The image keeps the index (newer ones must be at least as recent) but not
# the downloaded packages.
rm -rf "$ROOTFS/var/cache/wh"

"$WORMOS_ROOT/scripts/build-initramfs.sh"

# /var becomes its own subvolume; the root keeps an empty mount point.
mv "$ROOTFS/var" "$FSROOT/@var"
mkdir "$ROOTFS/var"

chown -R 0:0 "$FSROOT"
chmod 0700 root "$FSROOT/@var/roothome" "$FSROOT/@snapshots"
chmod 1777 tmp
chmod 0600 etc/shadow

msg "Root filesystem ready ($(du -sh "$ROOTFS" | cut -f1))"
