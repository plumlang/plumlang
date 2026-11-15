#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Check docs/reference.md: every ```plum block passes `plum check`, and a
# block followed by an ```output block prints exactly that when run.
#
#   sh tests/reference.sh [./plum]
set -eu
PLUM=${1:-./plum}
DOC=docs/reference.md
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Split the page into block-N.plum and block-N.out files.
awk -v dir="$work" '
    /^```plum$/   { n++; mode = "plum"; file = dir "/block-" n ".plum"; printf "" > file; next }
    /^```output$/ { if (n && last == n) { mode = "out"; file = dir "/block-" n ".out"; printf "" > file; next } }
    /^```$/       { if (mode == "plum") last = n; else last = 0; mode = ""; next }
    mode != ""    { print > file; next }
    NF            { last = 0 }
' "$DOC"

blocks=0 runs=0 failed=0
for src in "$work"/block-*.plum; do
    [ -e "$src" ] || { echo "reference: no plum blocks found in $DOC"; exit 1; }
    blocks=$((blocks + 1))
    n="$(basename "$src" .plum)"
    if ! "$PLUM" check "$src" > "$work/$n.check" 2>&1; then
        echo "reference: $n of $DOC fails plum check:"
        sed "s|$src|$DOC ($n)|" "$work/$n.check"
        failed=$((failed + 1))
        continue
    fi
    if [ -f "$work/$n.out" ]; then
        runs=$((runs + 1))
        # Shell comparison only: MSYS2 has no cmp or diff. tr: Windows builds
        # may end lines with \r\n.
        actual="$("$PLUM" run "$src" 2>&1 | tr -d '\r')"
        if [ "$actual" != "$(cat "$work/$n.out")" ]; then
            echo "reference: $n of $DOC printed something else."
            echo "--- expected"
            cat "$work/$n.out"
            echo "--- actual"
            printf '%s\n' "$actual"
            failed=$((failed + 1))
        fi
    fi
done

if [ "$failed" -gt 0 ]; then
    echo "reference: $failed of $blocks blocks failed"
    exit 1
fi
echo "reference: $blocks blocks pass plum check, $runs print what the page shows"
