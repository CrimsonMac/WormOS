#!/bin/bash
# Build the release images of WormOS VERSION into out/release:
#
#   wormos-VERSION-x86_64.iso         live system and installer (CD/DVD or USB
#                                     stick, UEFI or BIOS firmware)
#   wormos-VERSION-x86_64.qcow2       ready-made disk for QEMU, virt-manager
#   SHA256SUMS
#
# Release images differ from development images in two ways: root has no
# password until the first-boot setup sets one, and the disk is larger
# (DISK_MB, default 20 GiB; the files stay small, the disk grows on use).
#
# Usage: scripts/release.sh VERSION

. "$(dirname "$0")/env.sh"

VERSION=${1:?usage: scripts/release.sh VERSION}
DISK_MB=${DISK_MB:-20480}
DEST=$OUT/release
NAME=wormos-$VERSION-x86_64

command -v qemu-img > /dev/null || die "qemu-img is missing (apt install qemu-utils)"
grep -qx "VERSION_ID=$VERSION" "$WORMOS_ROOT/recipes/base-files/files/etc/os-release" ||
	die "os-release does not say VERSION_ID=$VERSION"

rm -rf "$DEST"
mkdir -p "$DEST"

WORMOS_RELEASE=1 "$WORMOS_ROOT/scripts/build-rootfs.sh"
IMAGE=$DEST/$NAME.img DISK_MB=$DISK_MB "$WORMOS_ROOT/scripts/mkimage.sh"

msg "Writing $NAME.qcow2"
qemu-img convert -c -O qcow2 "$DEST/$NAME.img" "$DEST/$NAME.qcow2"

rm -f "$DEST/$NAME.img"

ISO=$DEST/$NAME.iso "$WORMOS_ROOT/scripts/mkiso.sh"

(cd "$DEST" && sha256sum "$NAME.iso" "$NAME.qcow2" > SHA256SUMS)
msg "Release $VERSION ready in $DEST:"
ls -lh "$DEST"
