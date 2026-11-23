#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Run fuzz targets built by `make fuzz-build`, each for SECONDS, from the
# seeds of corpus.sh plus what earlier runs found (BUILD/corpus/NAME,
# kept between runs). A crash, leak, hang or sanitizer report is saved
# in BUILD/crashes/NAME and fails the run.
#
# Usage: sh tests/fuzz/run.sh BUILD SECONDS NAME...
#
# FUZZ_FLAGS adds libFuzzer options (say -fork=4 for four processes).

build=${1:?usage: run.sh BUILD SECONDS NAME...}
seconds=${2:?usage: run.sh BUILD SECONDS NAME...}
shift 2
here=$(dirname "$0")
# stack traces for UBSan reports too
UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
export UBSAN_OPTIONS

# seeds of this run of its own, so runs side by side don't clash
seeds="$build/seeds-$$"
sh "$here/corpus.sh" "$seeds"
trap 'rm -rf "$seeds"' EXIT

status=0
for t in "$@"; do
    mkdir -p "$build/corpus/$t" "$build/crashes/$t"
    dict=""
    case $t in
        lexer | parser | check | run) dict="-dict=$here/plum.dict" ;;
    esac
    echo "fuzz: $t for ${seconds}s"
    # shellcheck disable=SC2086 # the flags split on purpose
    if ! "$build/fuzz-$t" $dict -max_total_time="$seconds" -timeout=25 -rss_limit_mb=2560 \
        -malloc_limit_mb=1024 -max_len=16384 -print_final_stats=1 \
        -artifact_prefix="$build/crashes/$t/" $FUZZ_FLAGS \
        "$build/corpus/$t" "$seeds/$t" >"$build/fuzz-$t.log" 2>&1; then
        echo "fuzz: $t FAILED; see $build/fuzz-$t.log and $build/crashes/$t/"
        grep -E '^(==[0-9]+==ERROR|SUMMARY|.*runtime error)' "$build/fuzz-$t.log" | head -n 5
        status=1
    else
        grep -E '^stat::number_of_executed_units' "$build/fuzz-$t.log" | head -n 1
    fi
done
exit "$status"
