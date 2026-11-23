#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Smoke-test the WebAssembly build (make wasm) under Node: programs run in
# the browser's plum print what the native `plum run` prints, and an
# endless loop stops at the step limit.
#
#   sh tests/wasm.sh [./plum] [build/wasm/plum.js]
#
# The programs: the examples that need no network or other files, the
# blocks of docs/reference.md that show their output, and the golden
# tests (tests/lang) that run on their own without files, the network,
# commands or the clock. Skipped, with a message, without node or the
# wasm build.
set -u
PLUM=${1:-./plum}
PLUM_JS=${2:-build/wasm/plum.js}

if ! command -v node > /dev/null 2>&1; then
    echo "wasm: skipped (node is not installed)"
    exit 0
fi
if [ ! -f "$PLUM_JS" ]; then
    echo "wasm: skipped (no $PLUM_JS: run make wasm)"
    exit 0
fi

NO_COLOR=1
export NO_COLOR
unset FORCE_COLOR

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir "$work/blocks" "$work/wasm"

# Can this program run the same in the browser? Not with the network,
# files, commands, packages, imports of other files or the clock
# (sleeping is limited there), nor a golden test run some other way.
portable() {
    [ ! -f "$(dirname "$1")/plum.toml" ] &&
        ! grep -Eq '^seed (fs|net|http|tls|os)([. ]|$)|^seed \.|^# (command|module|pending|ignore):' "$1"
}

list="$work/list"
: > "$list"
for f in examples/*.plum; do
    portable "$f" && echo "$f" >> "$list"
done

# The reference's blocks that show their output (as tests/reference.sh).
awk -v dir="$work/blocks" '
    /^```plum$/   { n++; mode = "plum"; file = dir "/block-" n ".plum"; printf "" > file; next }
    /^```output$/ { if (n && last == n) { mode = "out"; file = dir "/block-" n ".out"; printf "" > file; next } }
    /^```$/       { if (mode == "plum") last = n; else last = 0; mode = ""; next }
    mode != ""    { print > file; next }
    NF            { last = 0 }
' docs/reference.md
for f in "$work"/blocks/block-*.plum; do
    [ -f "${f%.plum}.out" ] && portable "$f" && echo "$f" >> "$list"
done

for f in $(find tests/lang -name '*.plum' | sort); do
    if [ -f "${f%.plum}.out" ] || [ -f "${f%.plum}.err" ]; then
        portable "$f" && echo "$f" >> "$list"
    fi
done

# An endless loop and the sandbox, last.
printf 'plant i = 0\nwhile good {\n    i += 1\n}\n' > "$work/endless.plum"
printf 'seed fs\ndrop(fs.read("plum.toml"))\n' > "$work/sandbox.plum"
echo "$work/endless.plum" >> "$list"
echo "$work/sandbox.plum" >> "$list"

# shellcheck disable=SC2046 # one path per line, without spaces
if ! node tests/wasm/run.mjs "$PLUM_JS" "$work/wasm" $(cat "$list"); then
    echo "wasm: the JavaScript API failed"
    exit 1
fi

# Compare files exactly, with only the shell (as tests/golden.sh).
same() {
    [ "$(cat "$1"; echo .)" = "$(cat "$2"; echo .)" ]
}

pass=0 fail=0 n=0
total=$(wc -l < "$list")
while IFS= read -r f; do
    n=$((n + 1))
    got="$work/wasm/$n"
    case $f in
        */endless.plum)
            if [ "$(cat "$got.code")" = 1 ] && grep -q "stopped after .* steps" "$got.err"; then
                pass=$((pass + 1))
            else
                echo "wasm: the endless loop didn't stop at the step limit:"
                cat "$got.err"
                fail=$((fail + 1))
            fi
            continue ;;
        */sandbox.plum)
            if [ "$(cat "$got.code")" = 1 ] && grep -q "isn't allowed in this run (sandbox)" "$got.err"; then
                pass=$((pass + 1))
            else
                echo "wasm: reading a file wasn't refused:"
                cat "$got.err"
                fail=$((fail + 1))
            fi
            continue ;;
    esac
    # Native, with an empty standard input; messages name the program
    # main.plum, as the playground does.
    "$PLUM" run "$f" < /dev/null > "$work/native.out" 2> "$work/native.raw"
    echo "$?" > "$work/native.code"
    sed "s|$f|main.plum|g" "$work/native.raw" > "$work/native.err"
    if same "$work/native.out" "$got.out" && same "$work/native.err" "$got.err" &&
        same "$work/native.code" "$got.code"; then
        pass=$((pass + 1))
    else
        echo "wasm: $f prints something else in the browser (exit $(cat "$got.code"), native $(cat "$work/native.code"))"
        echo "--- native"
        cat "$work/native.out" "$work/native.err"
        echo "--- wasm"
        cat "$got.out" "$got.err"
        fail=$((fail + 1))
    fi
done < "$list"

echo "wasm: $pass of $total programs print the same as native plum, $fail failed"
[ "$fail" -eq 0 ]
