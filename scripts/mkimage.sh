#!/bin/bash
# Build a bootable UEFI disk image from the assembled root filesystem.
#
# Layout (GPT):
#   1  EFI system partition (FAT32)  GRUB
#   2  WormOS root (btrfs)           subvolumes @, @var, @home, @snapshots,
#                                    @wormholes
#
# Each partition is built as a separate file and then written into the
# disk image, so no loop devices are needed. Building the btrfs needs root
# (sudo) for one step: mkfs.btrfs reads file owners in a way fakeroot cannot
# intercept, so the owners and modes fakeroot recorded are applied for real
# to a temporary copy of the tree first.

. "$(dirname "$0")/env.sh"

STATE=$BUILD/fakeroot.state
[ -f "$STATE" ] && [ -d "$ROOTFS" ] || die "run scripts/build-rootfs.sh first"

IMAGE=$OUT/wormos.img
DISK_MB=${DISK_MB:-4096}
ESP_MB=128
SECTOR=512
MB=$((1024 * 1024 / SECTOR))     # sectors per MiB

esp_start=$MB
esp_size=$((ESP_MB * MB))
root_start=$((esp_start + esp_size))
root_size=$(((DISK_MB - 1) * MB - root_start))   # leave 1 MiB for the backup GPT

new_uuid() { cat /proc/sys/kernel/random/uuid; }
FS_UUID=$(new_uuid)
ROOT_PARTUUID=$(new_uuid)
ESP_PARTUUID=$(new_uuid)

TMP=$BUILD/image
sudo rm -rf "$TMP"
mkdir -p "$TMP" "$OUT"

KERNEL_CMDLINE="root=UUID=$FS_UUID rootflags=subvol=@,compress=zstd ro console=tty0 console=ttyS0,115200"

msg "Applying file ownership"
fakeroot -i "$STATE" -- find "$FSROOT" -printf '%U %G %m %y %P\0' > "$TMP/owners"
sudo rm -rf "$TMP/fsroot"
sudo cp -a "$FSROOT" "$TMP/fsroot"
sudo python3 -I "$WORMOS_ROOT/scripts/apply-ownership.py" "$TMP/owners" "$TMP/fsroot"

msg "Creating btrfs root filesystem"
truncate -s $((root_size * SECTOR)) "$TMP/root.img"
sudo mkfs.btrfs -q \
	-L WORMOS \
	-U "$FS_UUID" \
	--compress zstd \
	--rootdir "$TMP/fsroot" \
	--subvol rw:@ \
	--subvol rw:@var \
	--subvol rw:@home \
	--subvol rw:@snapshots \
	--subvol rw:@wormholes \
	"$TMP/root.img"
sudo rm -rf "$TMP/fsroot"

msg "Creating EFI system partition"
grub-mkimage -O x86_64-efi -p /EFI/BOOT -o "$TMP/BOOTX64.EFI" \
	part_gpt fat btrfs zstd normal linux search search_fs_uuid \
	configfile echo test ls cat sleep reboot halt \
	serial terminal all_video efi_gop font gfxterm

cat > "$TMP/grub.cfg" <<EOF
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output console serial

set timeout=3
set default=0

search --no-floppy --fs-uuid --set=root $FS_UUID
set wormos_cmdline="$KERNEL_CMDLINE"
export wormos_cmdline

menuentry "WormOS" {
	linux /@/boot/vmlinuz \$wormos_cmdline
	initrd /@/boot/initramfs.img
}

# Entries for recent snapshots, maintained by snap.
if [ -f /@snapshots/grub.cfg ]; then
	source /@snapshots/grub.cfg
fi
EOF

mkfs.fat -F 32 -n WORMOS-EFI -C "$TMP/esp.img" $((ESP_MB * 1024)) > /dev/null
mmd -i "$TMP/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$TMP/esp.img" "$TMP/BOOTX64.EFI" "$TMP/grub.cfg" ::/EFI/BOOT/

msg "Writing disk image"
rm -f "$IMAGE"
truncate -s ${DISK_MB}M "$IMAGE"
sfdisk -q "$IMAGE" <<EOF
label: gpt
unit: sectors
start=$esp_start, size=$esp_size, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=$ESP_PARTUUID, name="EFI"
start=$root_start, size=$root_size, type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709, uuid=$ROOT_PARTUUID, name="WormOS"
EOF
dd if="$TMP/esp.img" of="$IMAGE" bs=$SECTOR seek=$esp_start conv=notrunc,sparse status=none
dd if="$TMP/root.img" of="$IMAGE" bs=$SECTOR seek=$root_start conv=notrunc,sparse status=none
rm -rf "$TMP"

msg "Image ready: $IMAGE ($(du -h "$IMAGE" | cut -f1) used of ${DISK_MB} MiB)"
