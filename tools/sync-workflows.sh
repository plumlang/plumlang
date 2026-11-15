#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Keep the workflows the three Plumlang repositories share identical:
# plumlang (this one, the source), plum.elixpo and branding. The
# Elixpoo callers come from elixpo/agent.elixpo's distribution/workflows;
# the rest are the Elixpo CI files kept here.
#
#   sh tools/sync-workflows.sh ../plum.elixpo ../branding     copy them over
#   sh tools/sync-workflows.sh --check ../plum.elixpo ...           only report differences
#
# Each repository keeps its own files beside these (plumlang's build matrix,
# plum.elixpo's deploy, branding's extension publishing).
SHARED="elixpo-ci.yml quality-gate.yml pr-management.yml stale-management.yml
elixpoo-mentions.yml elixpoo-merge-memory.yml elixpoo-review.yml elixpoo-triage.yml"

here=$(dirname "$0")/..
check=0
if [ "$1" = "--check" ]; then
    check=1
    shift
fi
if [ $# -eq 0 ]; then
    echo "usage: sh tools/sync-workflows.sh [--check] <repo directory>..." >&2
    exit 2
fi
status=0
for repo in "$@"; do
    for f in $SHARED; do
        src="$here/.github/workflows/$f"
        dst="$repo/.github/workflows/$f"
        if [ -f "$dst" ] && [ "$(cat "$src"; printf .)" = "$(cat "$dst"; printf .)" ]; then
            continue
        fi
        if [ $check -eq 1 ]; then
            echo "out of sync: $dst"
            status=1
        else
            mkdir -p "$repo/.github/workflows"
            cp "$src" "$dst"
            echo "updated $dst"
        fi
    done
done
if [ $check -eq 1 ] && [ $status -eq 0 ]; then
    echo "workflows: in sync with $*"
fi
exit $status
