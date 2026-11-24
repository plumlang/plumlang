#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Fill the package-manager templates in packaging/ with a release's version
# and checksums:
#
#   sh tools/release/packaging.sh <version> <SHA256SUMS> <outdir>
#
# Writes <outdir>/homebrew/plum.rb, <outdir>/winget/manifests/e/Elixpo/
# Plumlang/<version>/*.yaml (the layout winget-pkgs expects),
# <outdir>/aur/PKGBUILD and .SRCINFO (the plum-bin AUR package) and
# <outdir>/scoop/plum.json. Fails if a checksum a template needs is missing.
# packaging/README.md says where each one goes.
set -eu
[ $# -eq 3 ] || { echo "usage: packaging.sh <version> <SHA256SUMS> <outdir>" >&2; exit 2; }
root="$(cd "$(dirname "$0")/../.." && pwd)"
version=$1 sums=$2 out=$3
date="$(date -u +%Y-%m-%d)"

# 2.1.4 and earlier were released under the Plumlang Commercial License
# only; later releases are Apache-2.0 OR the commercial license.
if [ "$(printf '%s\n' "$version" 2.1.4 | sort -V | head -n 1)" = "$version" ]; then
    license="LicenseRef-Plumlang-Commercial-1.0" scoop_license="Proprietary"
else
    license="Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0" scoop_license="Apache-2.0|Proprietary"
fi

fill() { # fill <template> <output>
    mkdir -p "$(dirname "$2")"
    script="s/@VERSION@/$version/g; s/@DATE@/$date/g; s/@LICENSE@/$license/g; s/@SCOOP_LICENSE@/$scoop_license/g"
    for target in linux-x86_64 linux-arm64 linux-riscv64 macos-universal windows-x86_64 windows-arm64; do
        case $target in windows-*) ext=zip ;; *) ext=tar.gz ;; esac
        sha="$(awk -v f="plum-$version-$target.$ext" '$2 == f || $2 == "*" f { print $1 }' "$sums")"
        [ -n "$sha" ] && script="$script; s/@SHA_$target@/$sha/g"
    done
    sed "$script" "$1" > "$2"
    if grep -n '@[A-Za-z_0-9-]*@' "$2" >&2; then
        echo "$2: a value above was not filled (is its archive in $sums?)" >&2
        exit 1
    fi
}

fill "$root/packaging/homebrew/plum.rb.in" "$out/homebrew/plum.rb"
w="$out/winget/manifests/e/Elixpo/Plumlang/$version"
for t in "$root"/packaging/winget/*.yaml.in; do
    f="$(basename "$t" .in)"
    fill "$t" "$w/$f"
done
fill "$root/packaging/aur/PKGBUILD.in" "$out/aur/PKGBUILD"
fill "$root/packaging/aur/.SRCINFO.in" "$out/aur/.SRCINFO"
fill "$root/packaging/scoop/plum.json.in" "$out/scoop/plum.json"
echo "$out/homebrew/plum.rb"
echo "$w"
echo "$out/aur"
echo "$out/scoop/plum.json"
