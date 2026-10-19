#!/bin/bash
# Build the initramfs into the assembled root filesystem (/boot/initramfs.img).
#
# The initramfs holds wormrd as /init, the snapshot tool for boot-time
# operations, a BusyBox rescue shell and the shared libraries they need.
# Files are taken from the root filesystem so they are already stripped.
# The archive is written with the kernel's gen_init_cpio, which sets owners
# and modes explicitly, so no root privileges are needed.

. "$(dirname "$0")/env.sh"

GEN_CPIO=$BUILD/kernel/usr/gen_init_cpio
[ -x "$GEN_CPIO" ] || die "gen_init_cpio missing, run scripts/build-kernel.sh first"
[ -x "$ROOTFS/usr/lib/wormrd/init" ] || die "wormrd is not installed in the root filesystem"

OUTPUT=$ROOTFS/boot/initramfs.img
LIST=$BUILD/initramfs.list

# Programs copied into the initramfs, and BusyBox applets for the rescue shell.
PROGRAMS=(/usr/bin/busybox /usr/bin/snap)
APPLETS=(sh ash cat ls mkdir mount umount ln cp mv rm dmesg less vi reboot poweroff)

declare -A seen=()
entries=()

add_dir() {
	[ -n "${seen[$1]:-}" ] && return
	seen[$1]=1
	entries+=("dir $1 0755 0 0")
}

add_file() {   # add_file PATH-IN-ROOTFS [MODE] [DEST]
	local src=$1 mode=${2:-0755} dest=${3:-$1}
	[ -n "${seen[$dest]:-}" ] && return
	seen[$dest]=1
	entries+=("file $dest $ROOTFS$src $mode 0 0")
}

# Add a program and, recursively, the libraries it links against.
add_elf() {
	local path=$1 lib
	add_file "$path"
	while read -r lib; do
		[ -e "$ROOTFS/usr/lib/$lib" ] || die "$path needs $lib, not found"
		[ -n "${seen[/usr/lib/$lib]:-}" ] || add_elf "/usr/lib/$lib"
	done < <("$TARGET-readelf" -d "$ROOTFS$path" |
		sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
}

for d in /dev /proc /sys /run /tmp /top /sysroot /usr /usr/bin /usr/lib; do
	add_dir "$d"
done
entries+=(
	"nod /dev/console 0600 0 0 c 5 1"
	"nod /dev/null 0666 0 0 c 1 3"
	"slink /bin usr/bin 0777 0 0"
	"slink /sbin usr/bin 0777 0 0"
	"slink /lib usr/lib 0777 0 0"
	"slink /lib64 usr/lib 0777 0 0"
)

add_file /usr/lib/wormrd/init 0755 /init
add_elf /usr/lib/ld-linux-x86-64.so.2
for p in "${PROGRAMS[@]}"; do
	if [ -e "$ROOTFS$p" ]; then
		add_elf "$p"
	else
		msg "Skipping $p (not installed)"
	fi
done
for a in "${APPLETS[@]}"; do
	entries+=("slink /usr/bin/$a busybox 0777 0 0")
done

printf '%s\n' "${entries[@]}" > "$LIST"
"$GEN_CPIO" "$LIST" | zstd -q -19 -T0 > "$OUTPUT.tmp"
mv "$OUTPUT.tmp" "$OUTPUT"

msg "Initramfs ready ($(du -h "$OUTPUT" | cut -f1))"
