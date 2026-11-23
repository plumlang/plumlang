#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Assemble the npm package @plumlang/plumlang from a wasm build:
#
#   sh tools/release/npm.sh <wasm folder> <version> <outdir>
#
# <wasm folder> holds plum.js and plum.wasm (build/wasm, or the folder in a
# release's plum-X.Y.Z-wasm.tar.gz). Writes packaging/npm with those two
# files and the LICENSE to <outdir>, with the package's version set to
# <version>, ready for npm pack or npm publish.
set -eu
[ $# -eq 3 ] || { echo "usage: npm.sh <wasm folder> <version> <outdir>" >&2; exit 2; }
root="$(cd "$(dirname "$0")/../.." && pwd)"
wasm=$1 version=$2 out=$3
case $version in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "npm.sh: not a version: $version" >&2; exit 2 ;;
esac
for f in plum.js plum.wasm; do
    [ -f "$wasm/$f" ] || { echo "npm.sh: no $f in $wasm" >&2; exit 1; }
done

mkdir -p "$out"
cp "$root/packaging/npm/package.json" "$root/packaging/npm/index.d.ts" \
    "$root/packaging/npm/README.md" "$root/LICENSE" \
    "$wasm/plum.js" "$wasm/plum.wasm" "$out/"
cd "$out"
npm version "$version" --no-git-tag-version --allow-same-version > /dev/null
got="$(node -p 'require("./package.json").version')"
[ "$got" = "$version" ] || { echo "npm.sh: package.json says $got, not $version" >&2; exit 1; }
echo "$out: @plumlang/plumlang $version"
