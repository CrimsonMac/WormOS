#!/bin/bash
# Boot the WormOS disk image in QEMU with UEFI firmware.
#
# Usage: scripts/run-qemu.sh [--gui] [--snapshot]
#
# By default the VM runs on the serial console in this terminal
# (press Ctrl-A X to quit QEMU). --gui opens a graphical window instead.
# --snapshot discards all disk writes when QEMU exits.

. "$(dirname "$0")/env.sh"

IMAGE=$OUT/wormos.img
[ -f "$IMAGE" ] || die "no image, run scripts/mkimage.sh first"

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS=$OUT/OVMF_VARS.fd
[ -f "$OVMF_VARS" ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

display=(-nographic)
extra=()
for arg in "$@"; do
	case $arg in
	--gui)      display=(-display gtk -serial mon:stdio) ;;
	--snapshot) extra+=(-snapshot) ;;
	*)          die "unknown option: $arg" ;;
	esac
done

accel=(-accel tcg)
[ -w /dev/kvm ] && accel=(-accel kvm -cpu host)

exec qemu-system-x86_64 \
	-machine q35 \
	"${accel[@]}" \
	-smp "${QEMU_CPUS:-4}" \
	-m "${QEMU_MEM:-1024}" \
	-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
	-drive if=pflash,format=raw,file="$OVMF_VARS" \
	-drive file="$IMAGE",format=raw,if=virtio \
	-netdev user,id=net0 \
	-device virtio-net-pci,netdev=net0 \
	-device virtio-rng-pci \
	"${display[@]}" "${extra[@]}"
