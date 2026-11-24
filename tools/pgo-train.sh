#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# The training run of `make release-pgo`: runs an instrumented plum on
# what a profile should be made of, so the rebuild knows which paths are
# hot. The benchmarks are the run loop, calls, arithmetic and strings; the
# examples add the standard library, classes and errors; `plum check` and
# `plum fmt --check` the front end. A few seconds in all.
#
# Usage: sh tools/pgo-train.sh ./plum

PLUM=${1:-./plum}
log=$(mktemp)
trap 'rm -f "$log"' EXIT

# Run a step quietly; if it fails, show its output and stop.
step() {
    if ! "$@" >"$log" 2>&1; then
        echo "pgo: training step failed: $*" >&2
        tail -n 30 "$log" >&2
        exit 1
    fi
}

for file in bench/*.plum; do
    step "$PLUM" run "$file"
done
step sh tests/golden.sh "$PLUM" examples
for file in examples/*.plum; do
    step "$PLUM" check "$file"
done
step "$PLUM" fmt --check examples src/stdlib/plum
echo "pgo: trained on bench/ and examples/"
