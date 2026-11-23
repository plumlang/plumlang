#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Package a built plum binary as a release archive:
#
#   sh tools/release/package.sh <binary> <target> <outdir>
#
# <target> is os-arch, e.g. linux-x86_64, macos-universal, windows-arm64,
# or wasm, for which <binary> is the folder make wasm wrote (build/wasm).
# Writes <outdir>/plum-<version>-<target>.tar.gz (.zip for windows), holding
# one folder with the binary (plum.js and plum.wasm for wasm, with
# docs/playground.md), the licence and the README.
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
    wasm) cp "$binary/plum.js" "$binary/plum.wasm" "$root/docs/playground.md" "$work/$name/" ;;
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
            # Windows runners: PowerShell 7 instead of zip (5.1 writes
            # backslashes into the paths)
            pwsh -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$(cygpath -w "$outdir")\\$name.zip'"
        fi
        echo "$outdir/$name.zip" ;;
    *)
        # Stable ordering and owners, so archives differ only when contents do.
        tar -cf - "$name" | gzip -9n > "$outdir/$name.tar.gz"
        echo "$outdir/$name.tar.gz" ;;
esac
