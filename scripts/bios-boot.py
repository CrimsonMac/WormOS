#!/usr/bin/env python3
"""Make a GPT disk image bootable with BIOS firmware (GRUB's i386-pc).

Usage: bios-boot.py IMAGE BOOT_IMG CORE_IMG LBA SECTORS

Does what grub-bios-setup does, on an image file: core.img goes into the
BIOS boot partition (SECTORS long, from sector LBA), and GRUB's boot
sector goes into the protective MBR, keeping its partition table. The
boot sector loads core.img's first sector (diskboot), whose block list
then names the rest of core.img.
"""

import struct
import sys

SECTOR = 512
KERNEL_SECTOR = 0x5c      # boot.img: LBA of core.img's first sector
DRIVE_CHECK = 0x66        # boot.img: jump that some BIOSes need skipped
MBR_CODE = 440            # boot code ends where the disk signature starts
LIST = SECTOR - 12        # diskboot.img: block list (start, length, segment)


def main():
    image, boot_img, core_img = sys.argv[1:4]
    lba, room = int(sys.argv[4]), int(sys.argv[5])
    with open(boot_img, "rb") as f:
        boot = bytearray(f.read())
    with open(core_img, "rb") as f:
        core = bytearray(f.read())
    assert len(boot) == SECTOR, "boot.img must be one sector"
    core += bytes(-len(core) % SECTOR)
    sectors = len(core) // SECTOR
    assert sectors <= room, f"core.img needs {sectors} sectors, the partition has {room}"

    struct.pack_into("<Q", boot, KERNEL_SECTOR, lba)
    boot[DRIVE_CHECK:DRIVE_CHECK + 2] = b"\x90\x90"
    struct.pack_into("<QH", core, LIST, lba + 1, sectors - 1)

    with open(image, "r+b") as f:
        f.seek(0)
        mbr = bytearray(f.read(SECTOR))
        assert mbr[510:512] == b"\x55\xaa" and mbr[450] == 0xEE, "no protective MBR"
        mbr[:MBR_CODE] = boot[:MBR_CODE]
        f.seek(0)
        f.write(mbr)
        f.seek(lba * SECTOR)
        f.write(core)
    print(f"core.img: {sectors} sectors at {lba}")


if __name__ == "__main__":
    main()
