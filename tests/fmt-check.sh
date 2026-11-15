#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# `plum fmt` keeps the meaning and is stable. For every .plum file under
# the directories given (tests/lang, examples and src/stdlib/plum by
# default) that parses without errors:
#
#   1. formatting the formatted text changes nothing (idempotent);
#   2. `plum ast` of the formatted text is `plum ast` of the original
#      (it prints no positions, so layout can't make a difference);
#   3. every comment of the original is in the formatted text.
#
# Usage: sh tests/fmt-check.sh ./plum [DIR...]

PLUM=${1:-./plum}
[ $# -gt 0 ] && shift
DIRS=${*:-tests/lang examples src/stdlib/plum}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Compare two files exactly, with only the shell (see golden.sh).
same_file() {
    a=$(cat "$1"; printf .)
    b=$(cat "$2"; printf .)
    [ "$a" = "$b" ]
}

show_diff() {
    if command -v diff >/dev/null 2>&1; then
        diff -u "$1" "$2" | sed '1,2d; s/^/    /' | head -n 30
    fi
}

# The comments of a file, one per line: from a '#' outside a string to
# the end of the line. Strings are followed well enough for the test
# files: "..." with \" escapes (an interpolation's inner strings come in
# pairs, so they leave the count even) and """...""" over several lines.
comments() {
    awk '
    {
        line = $0
        sub(/\r$/, "", line)
        n = length(line)
        i = 1
        while (i <= n) {
            c = substr(line, i, 1)
            if (triple) {
                if (substr(line, i, 3) == "\"\"\"") { triple = 0; i += 3; continue }
                i++
                continue
            }
            if (quote) {
                if (c == "\\") { i += 2; continue }
                if (c == "\"") { quote = 0 }
                i++
                continue
            }
            if (substr(line, i, 3) == "\"\"\"") { triple = 1; i += 3; continue }
            if (c == "\"") { quote = 1; i++; continue }
            if (c == "#") {
                text = substr(line, i)
                sub(/[ \t]+$/, "", text)
                print text
                break
            }
            i++
        }
        quote = 0
    }' "$1"
}

files=0
fail=0
for dir in $DIRS; do
    for file in $(find "$dir" -name '*.plum' | sort); do
        # Only files without syntax errors are formatted.
        "$PLUM" ast "$file" >"$tmp/ast1" 2>/dev/null || continue
        files=$((files + 1))
        if ! "$PLUM" fmt --stdout "$file" >"$tmp/once.plum" 2>"$tmp/err"; then
            echo "FAIL $file: plum fmt failed"
            sed 's/^/    /' "$tmp/err" | head -n 10
            fail=$((fail + 1))
            continue
        fi
        "$PLUM" fmt --stdout "$tmp/once.plum" >"$tmp/twice.plum" 2>"$tmp/err"
        if ! same_file "$tmp/once.plum" "$tmp/twice.plum"; then
            echo "FAIL $file: formatting twice changes it again"
            show_diff "$tmp/once.plum" "$tmp/twice.plum"
            fail=$((fail + 1))
            continue
        fi
        "$PLUM" ast "$tmp/once.plum" >"$tmp/ast2" 2>"$tmp/err"
        if ! same_file "$tmp/ast1" "$tmp/ast2"; then
            echo "FAIL $file: the formatted file parses differently"
            show_diff "$tmp/ast1" "$tmp/ast2"
            fail=$((fail + 1))
            continue
        fi
        comments "$file" >"$tmp/comments"
        missing=$(awk 'FNR == NR { text = text $0 "\n"; next }
                       index(text, $0) == 0 { print "    " $0 }' "$tmp/once.plum" "$tmp/comments")
        if [ -n "$missing" ]; then
            echo "FAIL $file: comments lost"
            echo "$missing" | head -n 10
            fail=$((fail + 1))
        fi
    done
done

# The examples and the modules written in Plumlang are kept formatted
# (tests/lang is not: its files try out every way of laying code out).
if [ -z "$*" ] && ! "$PLUM" fmt --check examples src/stdlib/plum >"$tmp/changed"; then
    echo "FAIL these files are not formatted; run: plum fmt examples src/stdlib/plum"
    sed 's/^/    /' "$tmp/changed"
    fail=$((fail + 1))
fi

echo "fmt: $files files formatted, idempotent, with the same syntax tree and every comment; $fail failed"
[ "$fail" -eq 0 ] && [ "$files" -gt 0 ]
