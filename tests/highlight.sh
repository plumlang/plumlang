#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# The shared highlighting sample stays in step with the lexer:
#
#   1. tests/highlight/keywords.txt is the lexer's keyword table
#      (tools/keywords.sh), and `plum` lexes every listed word as a keyword;
#   2. tests/highlight/sample.plum uses every keyword as a keyword, and every
#      kind of literal token: INT, FLOAT, STRING, STRING_HEAD/MID/TAIL,
#      FORMAT, and a """ string;
#   3. the sample checks without errors and is formatted.
#
# Editor grammars (tree-sitter-plum, the VS Code grammar, zed-plum,
# highlightjs-plum, pygments-plum) fetch both files in their CI, so a new
# keyword in the lexer fails here until the list is updated, and then in
# each grammar until the grammar knows it.
#
# Usage: sh tests/highlight.sh ./plum
PLUM=${1:-./plum}
here=$(dirname "$0")
root="$here/.."
dir="$here/highlight"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0
fail() {
    echo "highlight: $*"
    status=1
}

# 1. The published list is the lexer's table, word for word.
sh "$root/tools/keywords.sh" >"$tmp/lexer"
if [ "$(cat "$tmp/lexer"; printf .)" != "$(cat "$dir/keywords.txt"; printf .)" ]; then
    fail "tests/highlight/keywords.txt differs from the lexer's keywords;" \
        "run 'sh tools/keywords.sh > tests/highlight/keywords.txt'"
fi
# (Carriage returns are dropped from plum's output, for Windows builds.)
"$PLUM" tokens "$dir/keywords.txt" 2>&1 | tr -d '\r' >"$tmp/kw-tokens"
if grep -q ' IDENT ' "$tmp/kw-tokens"; then
    fail "plum lexes these listed words as names: $(grep ' IDENT ' "$tmp/kw-tokens" | sed 's/.* IDENT //' | tr '\n' ' ')"
fi
grep -q ' EOF' "$tmp/kw-tokens" || fail "plum tokens failed on keywords.txt"
[ "$(wc -l <"$tmp/lexer")" -gt 0 ] || fail "found no keywords in src/lexer/lexer.c"

# 2. Every keyword and every literal token kind occurs in the sample.
"$PLUM" tokens "$dir/sample.plum" 2>&1 | tr -d '\r' >"$tmp/tokens"
# The source word at each keyword token: "plant", "true", ...
awk 'NR == FNR { line[FNR] = $0; next }
     $2 ~ /^[A-Z_]+$/ && $2 != "IDENT" {
         split($1, at, ":")
         word = substr(line[at[1]], at[2])
         if (match(word, /^[a-z_]+/)) print substr(word, 1, RLENGTH)
     }' "$dir/sample.plum" "$tmp/tokens" | sort -u >"$tmp/used"
while read -r word; do
    grep -qx "$word" "$tmp/used" || fail "the keyword '$word' is not used in sample.plum"
done <"$dir/keywords.txt"
for kind in INT FLOAT STRING STRING_HEAD STRING_MID STRING_TAIL FORMAT; do
    grep -q "^[0-9]*:[0-9]* $kind\( \|$\)" "$tmp/tokens" || fail "sample.plum has no $kind token"
done
grep -q ' EOF' "$tmp/tokens" || fail "plum tokens failed on sample.plum"
grep -q '"""' "$dir/sample.plum" || fail "sample.plum has no \"\"\" string"

# 3. It is a valid, formatted program.
"$PLUM" check "$dir/sample.plum" >"$tmp/check" 2>&1 || { fail "plum check fails on sample.plum:"; sed 's/^/    /' "$tmp/check"; }
"$PLUM" fmt --check "$dir/sample.plum" >/dev/null 2>&1 || fail "sample.plum is not formatted; run 'plum fmt'"

[ "$status" -eq 0 ] &&
    echo "highlight: $(wc -l <"$dir/keywords.txt" | tr -d ' ') keywords match the lexer and the sample uses them all"
exit "$status"
