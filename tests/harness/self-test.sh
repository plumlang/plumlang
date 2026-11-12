#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Tests the golden-file runner against tests/harness/cases with a fake
# plum: 3 cases must pass, 1 be pending, and 4 fail (a wrong output, a
# wrong exit code, a stale pending marker, a missing expectation file).
here=$(dirname "$0")
out=$(sh "$here/../golden.sh" "$here/fake-plum.sh" "$here/cases" 2>&1)
status=$?
summary=$(printf '%s\n' "$out" | tail -n 1)
want="golden: 3 passed, 1 pending, 4 failed"
if [ "$status" -ne 0 ] && [ "$summary" = "$want" ]; then
    echo "harness: runner self-test passed"
    exit 0
fi
echo "harness: runner self-test FAILED (exit $status)"
echo "  expected: $want"
printf '%s\n' "$out" | sed 's/^/  /'
exit 1
