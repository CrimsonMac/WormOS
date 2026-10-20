# WormOS build entry points. See README.md for details.

# In build order: later packages build against earlier ones.
PACKAGES := base-files busybox worminit wormrd snap \
	zlib zstd openssl ca-certificates curl monocypher \
	libgpg-error npth libassuan libksba libgcrypt gnupg wormhole-keyrings \
	libmnl libnftnl nftables wh worm \
	libpcap tcpdump

all: image

toolchain:
	scripts/build-toolchain.sh

kernel: toolchain
	scripts/build-kernel.sh

packages: toolchain
	tools/wbuild $(PACKAGES)

repo: packages kernel
	tools/wrepo

rootfs: repo
	scripts/build-rootfs.sh

image: rootfs
	scripts/mkimage.sh

run:
	scripts/run-qemu.sh

run-gui:
	scripts/run-qemu.sh --gui

test:
	tests/boot-test.py

# Needs internet access: sets up the Debian and Kali wormholes.
test-wormholes:
	tests/wormhole-test.py

clean:
	rm -rf build/work build/pkgs build/pkginfo build/fsroot build/kernel build/image build/host out

distclean:
	rm -rf build out

.PHONY: all toolchain kernel packages repo rootfs image run run-gui test test-wormholes clean distclean
