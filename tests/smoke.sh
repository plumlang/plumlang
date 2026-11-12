#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Smoke tests for the `plum` command. The golden-file test harness (#7)
# replaces this with tests/**/*.plum and their expected output.
#
# Usage: sh tests/smoke.sh ./plum

PLUM=${1:-./plum}
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

# check NAME EXPECTED_EXIT GREP_PATTERN STREAM -- ARGS...
#   STREAM is out or err; GREP_PATTERN must appear in it ("": it must be empty).
check() {
    name=$1 want=$2 pattern=$3 stream=$4
    shift 5
    "$PLUM" "$@" >"$tmp/out" 2>"$tmp/err"
    got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL $name: exit $got, expected $want"
        sed 's/^/    /' "$tmp/err"
        fail=$((fail + 1))
        return
    fi
    if [ -z "$pattern" ]; then
        if [ -s "$tmp/$stream" ]; then
            echo "FAIL $name: std$stream should be empty"
            sed 's/^/    /' "$tmp/$stream"
            fail=$((fail + 1))
            return
        fi
    elif ! grep -q -- "$pattern" "$tmp/$stream"; then
        echo "FAIL $name: std$stream does not contain '$pattern'"
        sed 's/^/    /' "$tmp/$stream"
        fail=$((fail + 1))
        return
    fi
    pass=$((pass + 1))
}

printf 'drop("hello")\n' >"$tmp/hello.plum"

check "no arguments shows help"     0 "Usage:"              out --
check "help"                        0 "plum run <file>"     out -- help
check "--help"                      0 "Usage:"              out -- --help
check "version"                     0 "^plum [0-9]*\.[0-9]*\.[0-9]*" out -- version
check "--version"                   0 "^plum "              out -- --version
check "unknown command"             2 "unknown command 'frobnicate'" err -- frobnicate
check "run without a file"          2 "needs a file"        err -- run
check "run a missing file"          3 "cannot read"         err -- run "$tmp/missing.plum"
check "run checks the file"         1 "has no errors"       err -- run "$tmp/hello.plum"
check "file shorthand"              1 "has no errors"       err -- "$tmp/hello.plum"
check "check a correct file"        0 ""                    out -- check "$tmp/hello.plum"

echo "smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
