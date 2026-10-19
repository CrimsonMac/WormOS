#!/bin/bash
# Assemble the root filesystem from staged packages.
#
# Runs under fakeroot so files can be owned by root without real privileges.
# The ownership database is saved to build/fakeroot.state for mkimage.sh.

. "$(dirname "$0")/env.sh"

STATE=$BUILD/fakeroot.state
if [ -z "${FAKEROOTKEY:-}" ]; then
	rm -f "$STATE"
	exec fakeroot -s "$STATE" -- "$0" "$@"
fi

PACKAGES=(base-files glibc gcc-libs busybox worminit linux)

for p in "${PACKAGES[@]}"; do
	[ -d "$PKGDIR/$p" ] || die "package $p has not been built"
done

msg "Assembling root filesystem"
rm -rf "$FSROOT"
mkdir -p "$ROOTFS" "$FSROOT/@home" "$FSROOT/@snapshots"

cd "$ROOTFS"
mkdir -p boot dev etc home mnt proc root run sys tmp \
	usr/bin usr/lib usr/sbin usr/share \
	var/cache var/lib var/log var/spool
ln -s usr/bin bin
ln -s usr/sbin sbin
ln -s usr/lib lib
ln -s usr/lib lib64
ln -s lib usr/lib64
ln -s ../run var/run
ln -s ../run/lock var/lock
ln -s ../tmp var/tmp

for p in "${PACKAGES[@]}"; do
	case $p in
	glibc)
		# Runtime only: headers, static libraries and docs stay in the sysroot.
		rsync -a \
			--exclude=/usr/include \
			--exclude='*.a' --exclude='*.o' \
			--exclude=/usr/share/info --exclude=/usr/share/man \
			--exclude=/usr/share/i18n \
			"$PKGDIR/$p/" "$ROOTFS/"
		;;
	*)
		rsync -a "$PKGDIR/$p/" "$ROOTFS/"
		;;
	esac
done

msg "Stripping binaries"
find "$ROOTFS/usr" -type f \( -perm -u+x -o -name '*.so*' \) ! -name '*.ko*' |
while read -r f; do
	if [ "$(head -c4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then
		"$TARGET-strip" --strip-unneeded "$f" 2>/dev/null || true
	fi
done

chown -R 0:0 "$FSROOT"
chmod 0700 root
chmod 1777 tmp
chmod 0600 etc/shadow

msg "Root filesystem ready ($(du -sh "$ROOTFS" | cut -f1))"
