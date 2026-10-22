# WormOS build entry points. See README.md for details.

# In build order: later packages build against earlier ones.
PACKAGES := base-files busybox worminit wormrd snap \
	zlib zstd openssl ca-certificates curl monocypher \
	libgpg-error npth libassuan libksba libgcrypt gnupg wormhole-keyrings \
	libmnl libnftnl nftables wh worm bh expat unbound libevent tor \
	libpcap tcpdump dbus \
	kmod eudev seatd linux-firmware \
	libffi wayland wayland-protocols libdrm pixman xkeyboard-config \
	libxkbcommon libevdev mtdev libinput hwdata libdisplay-info mesa \
	libpng freetype fribidi fontconfig pcre2 glib harfbuzz libxml2 cairo pango \
	font-inter font-hack wlroots labwc tllist fcft foot grim swaybg wlr-randr xcompose \
	libtirpc libxcrypt open-vm-tools \
	libjpeg-turbo libtiff shared-mime-info gdk-pixbuf libepoxy graphene gtk4 \
	gtk4-layer-shell dconf hicolor-icon-theme adwaita-icon-theme wormos-gtk-theme worm-shell wormos-snapshots \
	alsa-lib libsndfile pipewire lua wireplumber \
	xorgproto xcb-proto libxau xtrans libxcb libx11 libxext libxrender libxrandr \
	atk gtk3 tor-browser-runtime tor-browser \
	wormos-login \
	wormos-desktop

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
	tests/desktop-test.py

# Needs internet access: sets up the Debian and Kali wormholes.
# DNS=ADDR uses that name server in the VM instead of QEMU's relay.
test-wormholes:
	tests/wormhole-test.py $(if $(DNS),--dns $(DNS))

# Needs internet access: installs Tor Browser's libraries, downloads the
# browser from torproject.org and starts it.
test-torbrowser:
	tests/torbrowser-test.py

# Release images in out/release (VERSION must match os-release).
release: repo
	scripts/release.sh $(VERSION)

# The public package repository: source archives, then the upload (gh).
sources: repo
	scripts/source-archives.sh

publish-repo: sources
	scripts/publish-repo.sh

clean:
	rm -rf build/work build/pkgs build/pkginfo build/fsroot build/kernel build/image build/host out

distclean:
	rm -rf build out

.PHONY: all toolchain kernel packages repo rootfs image run run-gui test test-wormholes test-torbrowser release sources publish-repo clean distclean
