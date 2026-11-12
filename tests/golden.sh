#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Golden-file tests: every tests/lang/**/NAME.plum is run with `plum run`
# from the repository root, and its output compared with
#
#   NAME.out   expected standard output; the program must exit 0
#   NAME.err   expected standard error;  the program must exit 1
#
# A test whose first line is `# pending: <reason>` is expected to fail
# until the feature lands: it is reported as pending and does not fail
# the run. A pending test that passes fails the run, so the marker is
# removed as soon as it is stale.
#
# Usage:
#   sh tests/golden.sh ./plum [DIR]       run the tests in DIR (tests/lang)
#   UPDATE=1 sh tests/golden.sh ./plum    rewrite NAME.out / NAME.err from
#                                         the actual output (not pending ones)
#
# Carriage returns are ignored when comparing, for Windows builds.

PLUM=${1:-./plum}
DIR=${2:-tests/lang}

# A stand-in written as a shell script (the runner's self-test) is run
# through sh, since executable bits don't survive every checkout.
run_plum() {
    case $PLUM in
        *.sh) sh "$PLUM" "$@" ;;
        *) "$PLUM" "$@" ;;
    esac
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
pending=0
fail=0

# Compare two files exactly, with only the shell: minimal Windows
# environments (MSYS2) have neither cmp nor diff. The trailing "." keeps
# $(...) from dropping final newlines, so they count too.
same_file() {
    a=$(cat "$1"; printf .)
    b=$(cat "$2"; printf .)
    [ "$a" = "$b" ]
}

show_diff() {
    if command -v diff >/dev/null 2>&1; then
        diff -u "$1" "$2" | sed '1,2d; s/^/    /' | head -n 40
    else
        echo "    expected:"; sed 's/^/      /' "$1" | head -n 20
        echo "    actual:"; sed 's/^/      /' "$2" | head -n 20
    fi
}

for test in $(find "$DIR" -name '*.plum' | sort); do
    base=${test%.plum}
    if [ -f "$base.out" ]; then
        expect="$base.out" stream=out want_exit=0
    elif [ -f "$base.err" ]; then
        expect="$base.err" stream=err want_exit=1
    else
        echo "FAIL $test: no $base.out or $base.err"
        fail=$((fail + 1))
        continue
    fi

    is_pending=0
    if head -n 1 "$test" | grep -q '^# pending:'; then
        is_pending=1
    fi

    run_plum run "$test" >"$tmp/out" 2>"$tmp/err"
    got_exit=$?
    tr -d '\r' <"$tmp/$stream" >"$tmp/actual"
    tr -d '\r' <"$expect" >"$tmp/expected"

    ok=1
    [ "$got_exit" -eq "$want_exit" ] || ok=0
    same_file "$tmp/expected" "$tmp/actual" || ok=0

    if [ "$is_pending" -eq 1 ]; then
        if [ "$ok" -eq 1 ]; then
            echo "FAIL $test: passes now; remove its '# pending:' line"
            fail=$((fail + 1))
        else
            pending=$((pending + 1))
        fi
        continue
    fi

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    elif [ -n "$UPDATE" ]; then
        cp "$tmp/actual" "$expect"
        echo "updated $expect"
        pass=$((pass + 1))
    else
        echo "FAIL $test"
        if [ "$got_exit" -ne "$want_exit" ]; then
            echo "    exit code $got_exit, expected $want_exit"
        fi
        show_diff "$tmp/expected" "$tmp/actual"
        if [ "$stream" = out ] && [ -s "$tmp/err" ]; then
            echo "    stderr:"; sed 's/^/      /' "$tmp/err" | head -n 10
        fi
        fail=$((fail + 1))
    fi
done

echo "golden: $pass passed, $pending pending, $fail failed"
[ "$fail" -eq 0 ]
