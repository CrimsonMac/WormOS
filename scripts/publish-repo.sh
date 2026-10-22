#!/bin/bash
# Publish the core package repository: out/repo (index, index.sig and the
# packages) and out/sources (from scripts/source-archives.sh) become the
# assets of the release "core" of the public packages repository, the URL
# in /etc/wh/repo. Assets no longer in either folder are removed, so the
# release always holds exactly one repository state.
#
# Needs the GitHub CLI (gh), logged in with write access to $PACKAGES_REPO.
#
# Usage: scripts/publish-repo.sh

. "$(dirname "$0")/env.sh"

PACKAGES_REPO=${PACKAGES_REPO:-CrimsonMac/wormos-packages}
TAG=core

command -v gh > /dev/null || die "gh is missing (the GitHub CLI)"
[ -f "$REPO/index" ] && [ -f "$REPO/index.sig" ] || die "no signed index in $REPO"
[ -f "$OUT/sources/SHA256SUMS" ] || die "no source archives (run scripts/source-archives.sh)"
grep -qF "github.com/$PACKAGES_REPO/releases/download/$TAG/" \
	"$WORMOS_ROOT/recipes/wh/recipe" || die "/etc/wh/repo does not point at $PACKAGES_REPO"

if ! gh release view "$TAG" -R "$PACKAGES_REPO" > /dev/null 2>&1; then
	msg "Creating the release $TAG"
	gh release create "$TAG" -R "$PACKAGES_REPO" --title "Core packages" \
		--notes "The WormOS core package repository, used by wh. Source archives (*.src.tar) hold each package's recipe and upstream sources."
fi

mapfile -t local_files < <(ls "$REPO"/*.wpkg "$REPO/index" "$REPO/index.sig" "$OUT/sources"/*)
mapfile -t remote < <(gh release view "$TAG" -R "$PACKAGES_REPO" --json assets -q '.assets[].name')

# Packages first, the index and its signature last: a client never sees an
# index naming packages that are not there yet.
msg "Uploading ${#local_files[@]} files"
for f in "${local_files[@]}"; do
	case $(basename "$f") in index|index.sig) continue ;; esac
	gh release upload "$TAG" -R "$PACKAGES_REPO" --clobber "$f"
done
gh release upload "$TAG" -R "$PACKAGES_REPO" --clobber "$REPO/index" "$REPO/index.sig"

declare -A keep
for f in "${local_files[@]}"; do
	keep[$(basename "$f")]=1
done
for name in "${remote[@]}"; do
	[ -n "${keep[$name]:-}" ] && continue
	msg "Removing $name"
	gh release delete-asset "$TAG" "$name" -R "$PACKAGES_REPO" -y
done
msg "Published to https://github.com/$PACKAGES_REPO/releases/tag/$TAG"
