#!/bin/bash
# Boot the WormOS disk image in QEMU with UEFI (or BIOS) firmware.
#
# Usage: scripts/run-qemu.sh [--gui] [--snapshot] [--bios]
#
# By default the VM runs on the serial console in this terminal
# (press Ctrl-A X to quit QEMU). --gui opens a graphical window instead.
# --snapshot discards all disk writes when QEMU exits. --bios boots with
# QEMU's default BIOS firmware (SeaBIOS) instead of UEFI.
# Both modes have a SPICE agent channel (QEMU's qemu-vdagent): the guest's
# clipboard agent shares the clipboard with QEMU's window and VNC clients.
# QEMU_HOSTFWD forwards host ports to the VM, e.g. tcp:127.0.0.1:2222-:22
# (several separated by spaces). QEMU_EXTRA adds options (split on spaces).
# IMAGE boots another image (raw, or qcow2 such as a release image).

. "$(dirname "$0")/env.sh"

IMAGE=${IMAGE:-$OUT/wormos.img}
[ -f "$IMAGE" ] || die "no image, run scripts/mkimage.sh first"
format=raw
case $IMAGE in *.qcow2) format=qcow2 ;; esac

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS=$OUT/OVMF_VARS.fd
[ -f "$OVMF_VARS" ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

display=(-nographic)
extra=()
firmware=(-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE"
	-drive if=pflash,format=raw,file="$OVMF_VARS")
for arg in "$@"; do
	case $arg in
	--gui)      display=(-display gtk -serial mon:stdio) ;;
	--snapshot) extra+=(-snapshot) ;;
	--bios)     firmware=() ;;
	*)          die "unknown option: $arg" ;;
	esac
done

netdev=user,id=net0
for fwd in ${QEMU_HOSTFWD:-}; do
	netdev+=,hostfwd=$fwd
done

accel=(-accel tcg)
[ -w /dev/kvm ] && accel=(-accel kvm -cpu host)

exec qemu-system-x86_64 \
	-machine q35 \
	"${accel[@]}" \
	-smp "${QEMU_CPUS:-4}" \
	-m "${QEMU_MEM:-1024}" \
	"${firmware[@]}" \
	-drive file="$IMAGE",format=$format,if=none,id=disk0 \
	-device virtio-blk-pci,drive=disk0,bootindex=0 \
	-netdev "$netdev" \
	-device virtio-net-pci,netdev=net0 \
	-device virtio-rng-pci \
	-device virtio-serial-pci,id=vserial0 \
	-chardev qemu-vdagent,id=vdagent0,name=vdagent,clipboard=on,mouse=off \
	-device virtserialport,bus=vserial0.0,chardev=vdagent0,name=com.redhat.spice.0 \
	"${display[@]}" "${extra[@]}" ${QEMU_EXTRA:-}
