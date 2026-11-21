#!/bin/bash
# Build WormOS's own programs with the build machine's compiler, warnings as
# errors, then run the unit tests (tests/unit). Needs no toolchain and no
# image: CI runs it on every push. The GUI programs need the GTK 4, gtk4-
# layer-shell and Wayland development files; without them they are skipped
# (CI installs them, so there nothing is skipped).
#
# Usage: scripts/ci-build.sh [CC]

. "$(dirname "$0")/env.sh"
set +u

CC=${1:-${CC:-cc}}
# Truncation warnings guess that a short name could be 100 KB long: shown,
# not fatal.
CFLAGS="-O2 -Werror -Wno-error=format-truncation"
[ "$(basename "$CC")" = clang ] && CFLAGS="$CFLAGS -Wno-unknown-warning-option"
dest=$BUILD/ci
mkdir -p "$dest"
work=$(mktemp -d "$dest/build.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Monocypher for wh, from its recipe's pinned source (as tools/wrepo does).
eval "$(sed -n '/^\(name\|version\|source\|sha256\)=/p' "$WORMOS_ROOT/recipes/monocypher/recipe")"
fetch "${source[0]}" "${sha256[0]}"
mono=$work/monocypher
unpack "$SOURCES/$(basename "${source[0]}")" "$mono"

cp -r "$WORMOS_ROOT/src" "$work/src"
failed=()
for dir in "$work"/src/*/; do
	name=$(basename "$dir")
	[ -f "$dir/Makefile" ] || continue
	if grep -q 'PKG_CONFIG' "$dir/Makefile" && ! pkg-config --exists gtk4 wayland-client 2>/dev/null; then
		msg "Skipped $name (no GTK 4 or Wayland development files)"
		continue
	fi
	msg "Building $name"
	make -s -C "$dir" CC="$CC" CFLAGS="$CFLAGS" \
		MONO_CFLAGS="-I$mono/src -I$mono/src/optional" \
		MONO_LIBS="$mono/src/monocypher.c $mono/src/optional/monocypher-ed25519.c" ||
		failed+=("$name")
done

msg "Unit tests"
cp -r "$WORMOS_ROOT/tests" "$work/tests"
make -s -C "$work/tests/unit" CC="$CC" check || failed+=(unit-tests)

[ ${#failed[@]} -eq 0 ] || die "failed: ${failed[*]}"
msg "All programs built, unit tests passed"
