#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# A stand-in for `plum run FILE`, used to test the golden-file runner
# itself: lines starting with "#> " go to stdout, "#! " to stderr, and
# "#exit N" sets the exit code.
[ "$1" = run ] || exit 2
code=0
while IFS= read -r line; do
    case $line in
        "#> "*) printf '%s\n' "${line#\#> }" ;;
        "#! "*) printf '%s\n' "${line#\#! }" >&2 ;;
        "#exit "*) code=${line#\#exit } ;;
    esac
done <"$2"
exit "$code"
