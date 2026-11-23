#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Language server tests: every tests/lsp/NAME.in is a recorded session,
# sent to `plum lsp`, and what the server answers is compared with
# NAME.out.
#
# In NAME.in, each line is one message body, which the script frames
# with its Content-Length header. Blank lines and lines starting with
# '#' are skipped. A line `@raw TEXT` sends TEXT as it is, after printf
# has read its escapes (\r, \n), for broken framing. `@ROOT@` stands for
# the file URI of tests/lsp, and `@DIR@` for its path, so a session can
# name files there without an absolute path in it.
#
# NAME.out has one message per line, without headers, with the same
# stand-ins put back and the server's version replaced by VERSION.
#
# The server must exit 0, or N when a `# exit: N` line comes first.
# Carriage returns are ignored when comparing, for Windows builds.
#
# Usage:
#   sh tests/lsp.sh ./plum
#   UPDATE=1 sh tests/lsp.sh ./plum    rewrite NAME.out from the actual output

PLUM=${1:-./plum}
DIR=$(cd "$(dirname "$0")/lsp" && pwd)

case $PLUM in
    /* | [A-Za-z]:*) ;;
    *) PLUM="$(pwd)/$PLUM" ;;
esac

# The directory as the server sees it: C:/x on Windows shells that have
# `pwd -W`, else the path itself.
SERVER_DIR=$( (cd "$DIR" && pwd -W) 2>/dev/null || echo "$DIR")
case $SERVER_DIR in
    /*) ROOT="file://$SERVER_DIR" ;;
    *) ROOT="file:///$SERVER_DIR" ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

same_file() {
    a=$(cat "$1"; printf .)
    b=$(cat "$2"; printf .)
    [ "$a" = "$b" ]
}

# sed patterns: escape what is special in a replacement or an address
escape() {
    printf '%s' "$1" | sed 's/[][\\/.*^$&|]/\\&/g'
}

root_re=$(escape "$ROOT")
dir_re=$(escape "$SERVER_DIR")

# Frame each message of a session.
frame() {
    sed -e "s|@ROOT@|$ROOT|g" -e "s|@DIR@|$SERVER_DIR|g" "$1" |
        while IFS= read -r line || [ -n "$line" ]; do
            case $line in
                '' | '#'*) continue ;;
                '@raw '*)
                    # shellcheck disable=SC2059
                    printf "${line#@raw }"
                    continue
                    ;;
            esac
            n=$(printf '%s' "$line" | LC_ALL=C wc -c)
            printf 'Content-Length: %d\r\n\r\n%s' $((n)) "$line"
        done
}

# One message per line: the headers go; the stand-ins come back.
unframe() {
    tr -d '\r' |
        sed -e 's/Content-Length: [0-9]*$//' -e '/^$/d' |
        sed -e "s|$root_re|@ROOT@|g" -e "s|$dir_re|@DIR@|g" \
            -e 's/"name":"plum","version":"[^"]*"/"name":"plum","version":"VERSION"/'
}

for input in "$DIR"/*.in; do
    name=$(basename "$input" .in)
    expect="$DIR/$name.out"
    want_exit=$(head -n 1 "$input" | sed -n 's/^# exit: *\([0-9]*\).*/\1/p')
    want_exit=${want_exit:-0}
    frame "$input" >"$tmp/input"
    (cd "$DIR" && "$PLUM" lsp <"$tmp/input" >"$tmp/raw" 2>"$tmp/stderr")
    got_exit=$?
    unframe <"$tmp/raw" >"$tmp/actual"
    if [ -s "$tmp/stderr" ]; then
        echo "FAIL lsp/$name: the server wrote to standard error"
        sed 's/^/    /' "$tmp/stderr" | head -n 20
        fail=$((fail + 1))
        continue
    fi
    if [ ! -f "$expect" ] && [ -z "$UPDATE" ]; then
        echo "FAIL lsp/$name: no $name.out"
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
        echo "FAIL lsp/$name"
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

echo "lsp: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
