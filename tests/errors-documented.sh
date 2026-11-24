#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Every error code used in src/ has a "### CODE" section in docs/errors.md,
# and every documented code is still used, or marked retired, or marked
# planned (reserved for a designed feature that isn't implemented yet). A
# planned code that src/ uses must drop its "(planned)" mark.
here=$(dirname "$0")
root="$here/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

grep -rhoE '"[EW][0-9]{4}"' "$root/src" | tr -d '"' | sort -u >"$tmp/used"
sed -n 's/^### \([EW][0-9]\{4\}\).*/\1/p' "$root/docs/errors.md" | sort -u >"$tmp/documented"
grep -E '^### [EW][0-9]{4} \(retired\)' "$root/docs/errors.md" | sed 's/^### \([EW][0-9]*\).*/\1/' \
    | sort -u >"$tmp/retired"
grep -E '^### [EW][0-9]{4} \(planned\)' "$root/docs/errors.md" | sed 's/^### \([EW][0-9]*\).*/\1/' \
    | sort -u >"$tmp/planned"

status=0
while read -r code; do
    grep -qx "$code" "$tmp/documented" || { echo "errors: $code is used in src/ but not documented in docs/errors.md"; status=1; }
done <"$tmp/used"
while read -r code; do
    grep -qx "$code" "$tmp/used" || grep -qx "$code" "$tmp/retired" || grep -qx "$code" "$tmp/planned" ||
        { echo "errors: $code is documented but no longer used; mark it '(retired)'"; status=1; }
done <"$tmp/documented"
while read -r code; do
    grep -qx "$code" "$tmp/used" &&
        { echo "errors: $code is used in src/ now; remove its '(planned)' mark in docs/errors.md"; status=1; }
done <"$tmp/planned"

[ "$status" -eq 0 ] && echo "errors: $(wc -l <"$tmp/used" | tr -d ' ') codes, all documented"
exit "$status"
