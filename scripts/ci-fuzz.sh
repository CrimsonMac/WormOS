#!/bin/bash
# Build the fuzz targets (tests/fuzz) with clang and libFuzzer and run each
# for FUZZ_SECONDS (default 60). Inputs that crash a target are left in
# tests/fuzz as crash-* (CI uploads them).
#
# Usage: [FUZZ_SECONDS=N] scripts/ci-fuzz.sh

. "$(dirname "$0")/env.sh"
set +u

seconds=${FUZZ_SECONDS:-60}
work=$(mktemp -d "${TMPDIR:-/tmp}/wormos-fuzz.XXXXXX")
trap 'rm -rf -- "$work"' EXIT

# Monocypher (wh's and snap's signatures), from its recipe's pinned source.
eval "$(sed -n '/^\(name\|version\|source\|sha256\)=/p' "$WORMOS_ROOT/recipes/monocypher/recipe")"
fetch "${source[0]}" "${sha256[0]}"
mono=$work/monocypher
unpack "$SOURCES/$(basename "${source[0]}")" "$mono"

msg "Fuzzing each target for $seconds s"
make -C "$WORMOS_ROOT/tests/fuzz" run SECONDS="$seconds" \
	MONO_CFLAGS="-I$mono/src -I$mono/src/optional" \
	MONO_LIBS="$mono/src/monocypher.c $mono/src/optional/monocypher-ed25519.c" ||
	die "a fuzz target failed (crash-* in tests/fuzz)"
msg "No fuzz target failed"
