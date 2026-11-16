#!/bin/bash
# Build this fork's kde-spectacle .deb in a Debian trixie container (podman), so no build
# dependencies are installed on the host. Builds the committed HEAD.
#
#   packaging/build-deb.sh [output dir]      (default: packaging/out)
#
# Then install it and keep apt from replacing it with Debian's version:
#   sudo apt install ./packaging/out/kde-spectacle_<version>_amd64.deb
#   sudo apt-mark hold kde-spectacle
# Bump the version in debian/changelog for each new build you install.
set -euo pipefail
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
out=$(realpath -m "${1:-$repo/packaging/out}")
image=spectacle-build

if ! git -C "$repo" diff --quiet HEAD; then
    echo "Note: uncommitted changes are not included (this builds HEAD)." >&2
fi
if ! podman image exists "$image"; then
    podman build -t "$image" -f "$repo/packaging/Containerfile" "$repo/packaging"
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/kde-spectacle"
git -C "$repo" archive HEAD | tar -x -C "$work/kde-spectacle"
podman run --rm -v "$work":/work:Z -w /work/kde-spectacle -e DEB_BUILD_OPTIONS="parallel=$(nproc)" \
    "$image" dpkg-buildpackage -b -uc -us > "$work/build.log" 2>&1 \
    || { tail -40 "$work/build.log"; echo "Build failed." >&2; exit 1; }

mkdir -p "$out"
cp "$work"/*.deb "$out"/
ls -1 "$out"/kde-spectacle_*.deb
