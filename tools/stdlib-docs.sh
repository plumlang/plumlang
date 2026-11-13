#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Rewrite the reference table of each docs/stdlib/MODULE.md from what
# `plum stdlib --markdown MODULE` prints, so the docs list exactly the
# functions the modules have. With --check, change nothing and fail if any
# page is out of date or missing.
#
# Usage: sh tools/stdlib-docs.sh ./plum [--check]
plum=${1:-./plum}
check=$2
root=$(dirname "$0")/..
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0
count=0
for module in $("$plum" stdlib | sed -n 's/^\([a-z_.]*\): .*/\1/p'); do
    page="$root/docs/stdlib/$module.md"
    count=$((count + 1))
    if [ ! -f "$page" ]; then
        echo "stdlib: docs/stdlib/$module.md is missing"
        status=1
        continue
    fi
    "$plum" stdlib --markdown "$module" >"$tmp/table"
    awk -v table="$tmp/table" '
        /<!-- reference:end -->/ { while ((getline line < table) > 0) print line; skip = 0 }
        !skip { print }
        /<!-- reference:begin -->/ { skip = 1 }' "$page" >"$tmp/page"
    if [ "$(cat "$page"; printf .)" != "$(cat "$tmp/page"; printf .)" ]; then
        if [ "$check" = "--check" ]; then
            echo "stdlib: docs/stdlib/$module.md is out of date; run 'make stdlib-doc'"
            status=1
        else
            cp "$tmp/page" "$page"
            echo "docs/stdlib/$module.md updated"
        fi
    fi
done
if [ "$check" = "--check" ] && [ $status -eq 0 ]; then
    echo "stdlib: $count modules, documented"
fi
exit $status
