# WormOS build entry points. See README.md for details.

PACKAGES := base-files busybox worminit wormrd

all: image

toolchain:
	scripts/build-toolchain.sh

kernel: toolchain
	scripts/build-kernel.sh

packages: toolchain
	tools/wbuild $(PACKAGES)

rootfs: packages kernel
	scripts/build-rootfs.sh

image: rootfs
	scripts/mkimage.sh

run:
	scripts/run-qemu.sh

run-gui:
	scripts/run-qemu.sh --gui

test:
	tests/boot-test.py

clean:
	rm -rf build/work build/pkgs build/pkginfo build/fsroot build/kernel build/image out

distclean:
	rm -rf build out

.PHONY: all toolchain kernel packages rootfs image run run-gui test clean distclean
