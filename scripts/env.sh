# Common build environment. Sourced by all build scripts.

set -euo pipefail

WORMOS_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

TARGET=x86_64-wormos-linux-gnu
ARCH=x86_64
JOBS=${JOBS:-$(nproc)}

SOURCES=$WORMOS_ROOT/sources
BUILD=$WORMOS_ROOT/build
OUT=$WORMOS_ROOT/out
TOOLS=$BUILD/tools          # cross toolchain prefix
SYSROOT=$BUILD/sysroot      # target headers and libraries used for linking
WORK=$BUILD/work            # unpacked sources and build trees
PKGDIR=$BUILD/pkgs          # staged package contents, one directory per package
FSROOT=$BUILD/fsroot        # top level of the btrfs image (one dir per subvolume)
ROOTFS=$FSROOT/@            # assembled root filesystem (the @ subvolume)

export PATH=$TOOLS/bin:/usr/sbin:/sbin:$PATH
export LC_ALL=C

msg() { printf '\033[1;35m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# fetch URL SHA256
# Download a source archive into $SOURCES (if missing) and verify its checksum.
fetch() {
	local url=$1 sum=$2 file
	file=$SOURCES/$(basename "$url")
	mkdir -p "$SOURCES"
	if [ ! -f "$file" ]; then
		msg "Downloading $(basename "$url")"
		curl -fL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	echo "$sum  $file" | sha256sum -c --quiet - ||
		die "checksum mismatch for $(basename "$file")"
}

# unpack ARCHIVE DESTDIR
# Extract an archive into a fresh directory, stripping the top-level folder.
unpack() {
	rm -rf "$2"
	mkdir -p "$2"
	tar -xf "$1" -C "$2" --strip-components=1
}

# stamp NAME: mark a build step done; done_step NAME: test whether it is.
stamp()     { mkdir -p "$BUILD/stamps"; touch "$BUILD/stamps/$1"; }
done_step() { [ -f "$BUILD/stamps/$1" ]; }
