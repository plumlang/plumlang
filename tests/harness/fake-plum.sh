#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# A stand-in for `plum run FILE` and `plum tokens FILE`, used to test the
# golden-file runner itself. For `run`, lines starting with "#> " go to
# stdout; for `tokens`, lines starting with "#t ". Lines starting with
# "#! " go to stderr, and "#exit N" sets the exit code.
case $1 in
    run) out='#> ' ;;
    tokens) out='#t ' ;;
    *) exit 2 ;;
esac
code=0
while IFS= read -r line; do
    case $line in
        "$out"*) printf '%s\n' "${line#"$out"}" ;;
        "#! "*) printf '%s\n' "${line#\#! }" >&2 ;;
        "#exit "*) code=${line#\#exit } ;;
    esac
done <"$2"
exit "$code"
