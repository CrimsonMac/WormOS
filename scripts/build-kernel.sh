#!/bin/bash
# Build the WormOS kernel and stage it as the "linux" package.

. "$(dirname "$0")/env.sh"

LINUX=linux-6.18.57
KOBJ=$BUILD/kernel

[ -x "$TOOLS/bin/$TARGET-gcc" ] || die "toolchain missing, run scripts/build-toolchain.sh first"

if [ ! -f "$WORK/linux/Makefile" ]; then
	unpack "$SOURCES/$LINUX.tar.xz" "$WORK/linux"
fi

kmake() {
	make -C "$WORK/linux" O="$KOBJ" ARCH=$ARCH CROSS_COMPILE="$TARGET-" "$@"
}

msg "Configuring $LINUX"
mkdir -p "$KOBJ"
kmake x86_64_defconfig
"$WORK/linux/scripts/kconfig/merge_config.sh" -m -O "$KOBJ" \
	"$KOBJ/.config" "$WORMOS_ROOT/kernel/wormos.config"
kmake olddefconfig

# Report any requested option that did not survive dependency resolution.
missing=0
while IFS= read -r line; do
	case $line in CONFIG_*) ;; *) continue ;; esac
	if ! grep -qxF "$line" "$KOBJ/.config"; then
		echo "warning: not applied: $line"
		missing=1
	fi
done < "$WORMOS_ROOT/kernel/wormos.config"

msg "Building $LINUX"
kmake -j"$JOBS" bzImage modules

kver=$(kmake -s kernelrelease)
rm -rf "$PKGDIR/linux"
mkdir -p "$PKGDIR/linux/boot"
cp "$KOBJ/arch/x86/boot/bzImage" "$PKGDIR/linux/boot/vmlinuz-$kver"
cp "$KOBJ/.config" "$PKGDIR/linux/boot/config-$kver"
ln -sfn "vmlinuz-$kver" "$PKGDIR/linux/boot/vmlinuz"
kmake INSTALL_MOD_PATH="$PKGDIR/linux/usr" INSTALL_MOD_STRIP=1 modules_install
rm -f "$PKGDIR/linux/usr/lib/modules/$kver/build"

msg "Kernel $kver ready"
[ $missing -eq 0 ] || msg "Some config options were not applied (see warnings above)"
