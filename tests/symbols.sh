#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# No function of ours may share its name with one the binary imports
# from the system's libraries, even a static one.
#
# A static `send` in src/lsp/server.c once took the place of the socket
# library's send() for src/stdlib/net.c in Apple Clang's LTO builds: macOS
# declares send() with an assembler name (__DARWIN_ALIAS_C), so after the
# files were merged the call bound to the static function of the same name,
# and every net.write() crashed (#173). Other compilers and builds get it
# right, so the clash is caught here, by name, on every platform.
#
# Usage: sh tests/symbols.sh BUILD_DIR BINARY

LC_ALL=C
export LC_ALL

BUILD=${1:?usage: sh tests/symbols.sh BUILD_DIR BINARY}
BIN=${2:?usage: sh tests/symbols.sh BUILD_DIR BINARY}

if ! command -v nm >/dev/null 2>&1; then
    echo "symbols: skipped (no nm)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Function names of an nm listing, without Mach-O's leading underscore or
# glibc's @version.
names() {
    sed -e 's/@.*//' -e 's/^_//' | sort -u
}

# What the binary imports: undefined in it. (An LTO build's objects are
# bitcode that the system's nm may not read; the binary is enough then.)
nm -u "$BIN" 2>/dev/null | awk '{ print $NF }' | names > "$tmp/imports"
if [ ! -s "$tmp/imports" ]; then
    echo "symbols: skipped (nm lists no imports of $BIN)"
    exit 0
fi

# Every function we define, static ones included (our sources, not vendor/).
find "$BUILD/src" -name '*.o' | sort | while read -r o; do
    nm "$o" 2>/dev/null | awk '$2 == "t" || $2 == "T" { print $3 }'
done | names > "$tmp/ours"

clash=$(comm -12 "$tmp/imports" "$tmp/ours")
if [ -n "$clash" ]; then
    echo "symbols: functions named like ones $BIN imports from the system:"
    for name in $clash; do
        printf '  %s, defined in' "$name"
        find "$BUILD/src" -name '*.o' | sort | while read -r o; do
            if nm "$o" 2>/dev/null | awk '$2 == "t" || $2 == "T" { print $3 }' | names |
                grep -qx "$name"; then
                printf ' %s' "${o#"$BUILD"/}"
            fi
        done
        echo
    done
    echo "  rename them (a static function too: see tests/symbols.sh)"
    exit 1
fi
echo "symbols: ok ($(wc -l < "$tmp/ours" | tr -d ' ') functions, none named like an import)"
