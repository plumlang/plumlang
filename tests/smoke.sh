#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
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
#   Standard input is $STDIN, or empty.
check() {
    name=$1 want=$2 pattern=$3 stream=$4
    shift 5
    "$PLUM" "$@" <"${STDIN:-/dev/null}" >"$tmp/out" 2>"$tmp/err"
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
check "run a program"               0 "^hello$"             out -- run "$tmp/hello.plum"
check "file shorthand"              0 "^hello$"             out -- "$tmp/hello.plum"
check "check a correct file"        0 ""                    out -- check "$tmp/hello.plum"
check "run a package directory"     0 "^tallest: sunflower" out -- run "$here/../examples/garden"
check "a directory without plum.toml" 3 "without a plum.toml" err -- run "$tmp"
check "an unknown run option"       2 "unknown option"      err -- run --fast "$tmp/hello.plum"
check "the leak check"              0 "every object was freed" err -- run --check-leaks "$tmp/hello.plum"
check "memory statistics"           0 "^  allocations  *[0-9]" err -- run --mem-stats "$tmp/hello.plum"
check "memory statistics by kind"   0 "^  strings  *[0-9]"  err -- run --mem-stats "$tmp/hello.plum"
check "statistics and the leak check" 0 "every object was freed" err -- run --mem-stats --check-leaks "$tmp/hello.plum"
check "no statistics without the flag" 0 ""                err -- run "$tmp/hello.plum"

printf 'plant   x=1\n' >"$tmp/messy.plum"
check "help lists fmt"              0 "plum fmt"            out -- help
check "fmt without paths"           2 "needs files"         err -- fmt
check "fmt --check finds a change"  1 "messy.plum$"         out -- fmt --check "$tmp/messy.plum"
check "fmt formats in place"        0 ""                    out -- fmt "$tmp/messy.plum"
check "fmt --check after formatting" 0 ""                   out -- fmt --check "$tmp/messy.plum"
check "fmt --check a directory"     0 ""                    out -- fmt --check "$tmp"
check "fmt --stdout"                0 "^plant x = 1$"       out -- fmt --stdout "$tmp/messy.plum"
check "help lists test"             0 "plum test"           out -- help
check "test a package directory"    0 "^5 passed, 0 failed$" out -- test "$here/../examples/garden"
check "test finds files in subdirectories" 0 "garden_test.plum$" out -- test "$here/../examples"
check "test a filter that matches nothing" 0 "no tests match 'nothing'" out -- test --filter nothing "$here/../examples"
check "test a directory without tests" 0 "no tests found"  out -- test "$tmp"
check "test a missing file"         3 "cannot read"         err -- test "$tmp/missing_test.plum"
check "test an unknown option"      2 "unknown option"      err -- test --fast
check "test --filter without text"  2 "after '--filter'"    err -- test --filter
plum_abs=$(cd "$(dirname "$PLUM")" && pwd)/$(basename "$PLUM")
if (cd "$here/../examples/garden" && "$plum_abs" test >"$tmp/out" 2>&1) &&
    grep -q "^5 passed" "$tmp/out"; then
    pass=$((pass + 1))
else
    echo "FAIL test in a package's directory"
    sed 's/^/    /' "$tmp/out"
    fail=$((fail + 1))
fi

printf 'plant x = 20\nx * 2 + 2\n:type x\nexit(4)\n' >"$tmp/repl.in"
printf 'plant x = 1 +\n  "two"\n:q\n' >"$tmp/repl-error.in"
STDIN="$tmp/repl.in" check "repl echoes a value"   4 "^= 42 : int$"  out -- repl
STDIN="$tmp/repl.in" check "repl prints a type"    4 "^int$"         out -- repl
STDIN="$tmp/repl.in" check "repl without prompts"  4 ""              err -- repl
STDIN="$tmp/repl-error.in" check "repl reports an error" 0 "E0403" err -- repl

echo "smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
