#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Build the seed corpora of the fuzz targets into DIR/NAME, from the
# repository's own tests and examples (nothing large is committed).
#
# Usage: sh tests/fuzz/corpus.sh DIR

dir=${1:?usage: corpus.sh DIR}
here=$(dirname "$0")
root="$here/../.."

rm -rf "$dir"
for t in lexer parser check run json format http; do
    mkdir -p "$dir/$t"
done

# Programs: every .plum in the tests, the examples and the standard library.
n=0
for f in $(find "$root/tests" "$root/examples" "$root/src/stdlib/plum" -name '*.plum' | sort); do
    n=$((n + 1))
    for t in lexer parser check run; do
        cp "$f" "$dir/$t/$n.plum"
    done
done

# JSON: the documents the tests use, and a few made here.
i=0
for doc in '{}' '[]' 'null' 'true' '0' '-0.0e+1' '"aé😀\n"' \
    '{"a": [1, 2.5, "x", null, false], "b": {"c": {}}}' '[[[[[[[[1]]]]]]]]' \
    '{"a": 1, "a": 2}' '1e999' '123456789012345678901234567890' '"\ud800"'; do
    i=$((i + 1))
    printf '%s' "$doc" >"$dir/json/$i.json"
done
for f in $(find "$root/tests" "$root/examples" -name '*.json' | sort); do
    i=$((i + 1))
    cp "$f" "$dir/json/$i.json"
done

# Formats: a value kind byte, eight bytes of number, the format, a zero
# byte and a string (fuzz_format.c).
i=0
for kind in 0 1 2 5; do
    for fmt in '' '.2' '5' '<10' '>10' '^9' '*^9' 'x' 'X' 'o' 'b' 'e' '.3f' '08.2' 'd' '.0'; do
        i=$((i + 1))
        printf "\\$kind\\001\\002\\003\\004\\005\\006\\007\\010%s\\000h\\303\\251llo" "$fmt" \
            >"$dir/format/$i"
    done
done

# HTTP: requests and responses.
i=0
for msg in 'GET /a/7?x=1&y=%20 HTTP/1.1\r\nhost: x\r\n\r\n' \
    'POST /j HTTP/1.1\r\ncontent-length: 7\r\n\r\n{"a":1}' \
    'POST /j HTTP/1.1\r\ntransfer-encoding: chunked\r\n\r\n3\r\n[1]\r\n0\r\n\r\n' \
    'GET /a/1 HTTP/1.1\r\n\r\nGET /a/2 HTTP/1.0\r\nconnection: close\r\n\r\n' \
    'HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello' \
    'HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n' \
    'HTTP/1.1 404 Not Found\r\n\r\nrest of the body' \
    'http://example.com:8080/path?q=1'; do
    i=$((i + 1))
    printf '%b' "$msg" >"$dir/http/$i"
done
