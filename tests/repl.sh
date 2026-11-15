#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# REPL tests: every tests/repl/NAME.in is piped into `plum repl`, run
# from tests/repl (so `seed ./file` finds the modules there), and what it
# prints, standard output and error together, is compared with NAME.out.
#
# The REPL must exit 0, or N when a `# exit: N` line comes first.
# Carriage returns are ignored when comparing, for Windows builds.
#
# Usage:
#   sh tests/repl.sh ./plum
#   UPDATE=1 sh tests/repl.sh ./plum    rewrite NAME.out from the actual output

PLUM=${1:-./plum}
DIR=$(cd "$(dirname "$0")/repl" && pwd)

# The tests run in another directory: make the path to plum absolute.
case $PLUM in
    /* | [A-Za-z]:*) ;;
    *) PLUM="$(pwd)/$PLUM" ;;
esac

NO_COLOR=1
export NO_COLOR
unset FORCE_COLOR
# Nothing a test does goes into the history file.
PLUM_HISTORY=
export PLUM_HISTORY

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

same_file() {
    a=$(cat "$1"; printf .)
    b=$(cat "$2"; printf .)
    [ "$a" = "$b" ]
}

for input in "$DIR"/*.in; do
    name=$(basename "$input" .in)
    expect="$DIR/$name.out"
    want_exit=$(head -n 1 "$input" | sed -n 's/^# exit: *\([0-9]*\).*/\1/p')
    want_exit=${want_exit:-0}
    (cd "$DIR" && "$PLUM" repl <"$input" >"$tmp/raw" 2>&1)
    got_exit=$?
    tr -d '\r' <"$tmp/raw" >"$tmp/actual"
    if [ ! -f "$expect" ] && [ -z "$UPDATE" ]; then
        echo "FAIL repl/$name: no $name.out"
        fail=$((fail + 1))
        continue
    fi
    : >"$tmp/expected"
    if [ -f "$expect" ]; then
        tr -d '\r' <"$expect" >"$tmp/expected"
    fi
    if [ "$got_exit" -eq "$want_exit" ] && [ -f "$expect" ] &&
        same_file "$tmp/actual" "$tmp/expected"; then
        pass=$((pass + 1))
    elif [ -n "$UPDATE" ] && [ "$got_exit" -eq "$want_exit" ]; then
        cp "$tmp/actual" "$expect"
        echo "updated $expect"
        pass=$((pass + 1))
    else
        echo "FAIL repl/$name"
        if [ "$got_exit" -ne "$want_exit" ]; then
            echo "    exit code $got_exit, expected $want_exit"
        fi
        if command -v diff >/dev/null 2>&1; then
            diff -u "$tmp/expected" "$tmp/actual" | sed '1,2d; s/^/    /' | head -n 40
        else
            echo "    actual:"; sed 's/^/      /' "$tmp/actual" | head -n 40
        fi
        fail=$((fail + 1))
    fi
done

echo "repl: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
