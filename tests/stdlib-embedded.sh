#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# src/stdlib/embedded.c must match src/stdlib/plum/*.plum.
# Regenerate it with `make stdlib-embed`.
here=$(dirname "$0")
root="$here/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sh "$root/tools/embed-stdlib.sh" "$root/src/stdlib/plum" >"$tmp/embedded.c"
if [ "$(cat "$tmp/embedded.c"; printf .)" = "$(cat "$root/src/stdlib/embedded.c"; printf .)" ]; then
    echo "stdlib: modules written in Plumlang are embedded ($(ls "$root"/src/stdlib/plum/*.plum | wc -l | tr -d ' '))"
    exit 0
fi
echo "stdlib: src/stdlib/embedded.c is out of date; run 'make stdlib-embed'"
exit 1
