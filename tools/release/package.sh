#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Package a built plum binary as a release archive:
#
#   sh tools/release/package.sh <binary> <target> <outdir>
#
# <target> is os-arch, e.g. linux-x86_64, macos-universal, windows-arm64.
# Writes <outdir>/plum-<version>-<target>.tar.gz (.zip for windows), holding
# one folder with the binary, the licence and the README.
set -eu
[ $# -eq 3 ] || { echo "usage: package.sh <binary> <target> <outdir>" >&2; exit 2; }
root="$(cd "$(dirname "$0")/../.." && pwd)"
binary=$1 target=$2 outdir=$3
version="$(sh "$root/tools/release/version.sh")"
name="plum-$version-$target"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/$name" "$outdir"
case $target in
    windows-*) cp "$binary" "$work/$name/plum.exe" ;;
    *) cp "$binary" "$work/$name/plum"; chmod 755 "$work/$name/plum" ;;
esac
cp "$root/LICENSE" "$root/README.md" "$root/CHANGELOG.md" "$work/$name/"
cp -R "$root/LICENSES" "$work/$name/LICENSES"

outdir="$(cd "$outdir" && pwd)"
cd "$work"
case $target in
    windows-*)
        if command -v zip >/dev/null 2>&1; then
            zip -qr "$outdir/$name.zip" "$name"
        else
            # Windows runners: PowerShell instead of zip
            powershell -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$(cygpath -w "$outdir")\\$name.zip'"
        fi
        echo "$outdir/$name.zip" ;;
    *)
        # Stable ordering and owners, so archives differ only when contents do.
        tar -cf - "$name" | gzip -9n > "$outdir/$name.tar.gz"
        echo "$outdir/$name.tar.gz" ;;
esac
