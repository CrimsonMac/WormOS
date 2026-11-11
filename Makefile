# WormOS build entry points. See README.md for details.

# In build order: later packages build against earlier ones.
PACKAGES := base-files busybox worminit wormrd snap \
	zlib zstd openssl ca-certificates curl monocypher \
	libgpg-error npth libassuan libksba libgcrypt gnupg wormhole-keyrings \
	libmnl libnftnl nftables libxcrypt wh worm bh expat unbound libevent tor \
	libpcap tcpdump dbus \
	kmod eudev seatd linux-firmware \
	libffi wayland wayland-protocols libdrm pixman xkeyboard-config \
	libxkbcommon libevdev mtdev libinput hwdata libdisplay-info mesa \
	libpng freetype fribidi fontconfig pcre2 glib harfbuzz libxml2 cairo pango \
	font-inter font-hack wlroots labwc tllist fcft foot grim swaybg wlr-randr xcompose clip \
	libtirpc open-vm-tools \
	libjpeg-turbo libtiff shared-mime-info gdk-pixbuf libepoxy graphene gtk4 \
	gtk4-layer-shell dconf hicolor-icon-theme adwaita-icon-theme wormos-gtk-theme \
	alsa-lib libsndfile pipewire lua wireplumber worm-shell wormos-snapshots \
	xorgproto xcb-proto libxau xtrans libxcb libx11 libxext libxfixes libxrender libxrandr \
	atk gtk3 tor-browser-runtime tor-browser \
	ncurses zsh-autosuggestions zsh-syntax-highlighting zsh \
	xz bzip2 readline bash less nano vim htop tmux file tree ncdu jq lsof strace \
	wireguard-tools sqlite libarchive git openssh rsync python nmap binutils \
	tor-browser-bundle \
	wormos-desktop \
	wormos-login

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

# Upgrades an older release image (FROM=wormos-1.3-x86_64.qcow2) to out/repo.
test-upgrade:
	tests/upgrade-test.py $(FROM)

# On a Windows PC, from WSL: VMware Workstation, and QEMU for Windows.
# VMWARE_ARGS passes options (such as --no-3d or --firmware bios).
test-vmware:
	tests/vmware-test.py $(VMWARE_ARGS)

test-qemu-windows:
	tests/qemu-windows-test.py

# Release images in out/release (VERSION must match os-release).
release: repo
	scripts/release.sh $(VERSION)

# The public package repository: source archives, then the upload (gh).
sources: repo
	scripts/source-archives.sh

publish-repo: sources
	scripts/publish-repo.sh

clean:
	rm -rf build/work build/pkgs build/pkginfo build/cache build/fsroot build/kernel build/image build/host out

distclean:
	rm -rf build out

.PHONY: all toolchain kernel packages repo rootfs image run run-gui test test-wormholes test-torbrowser test-upgrade test-vmware test-qemu-windows release sources publish-repo clean distclean
