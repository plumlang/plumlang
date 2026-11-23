#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Keep the .github files the Plumlang repositories share identical:
# plumlang (this one, the source), plum.elixpo, branding, registry,
# tree-sitter-plum and setup-plum. That's the
# community files (CODEOWNERS, issue forms, the pull request template) and
# the shared workflows: Elixpo CI, and the Elixpoo callers from
# elixpo/agent.elixpo's distribution/workflows.
#
#   sh tools/sync-github.sh ../plum.elixpo ../branding ../registry \
#       ../tree-sitter-plum ../setup-plum                           copy them over
#   sh tools/sync-github.sh --check ../plum.elixpo ...          only report differences
#
# Each repository keeps its own files beside these: plumlang's build matrix,
# scorecard and language/library proposal forms; plum.elixpo's deploy and
# Search Console sync; branding's extension publishing; registry's publish
# and validate workflows; tree-sitter-plum's grammar CI; setup-plum's self-test.
SHARED="CODEOWNERS pull_request_template.md
ISSUE_TEMPLATE/config.yml ISSUE_TEMPLATE/bug_report.yml ISSUE_TEMPLATE/feature_request.yml
workflows/elixpo-ci.yml workflows/quality-gate.yml workflows/pr-management.yml
workflows/stale-management.yml workflows/elixpoo-mentions.yml
workflows/elixpoo-merge-memory.yml workflows/elixpoo-review.yml workflows/elixpoo-triage.yml"

here=$(dirname "$0")/..
check=0
if [ "$1" = "--check" ]; then
    check=1
    shift
fi
if [ $# -eq 0 ]; then
    echo "usage: sh tools/sync-github.sh [--check] <repo directory>..." >&2
    exit 2
fi
status=0
for repo in "$@"; do
    for f in $SHARED; do
        src="$here/.github/$f"
        dst="$repo/.github/$f"
        if [ -f "$dst" ] && [ "$(cat "$src"; printf .)" = "$(cat "$dst"; printf .)" ]; then
            continue
        fi
        if [ $check -eq 1 ]; then
            echo "out of sync: $dst"
            status=1
        else
            mkdir -p "$(dirname "$dst")"
            cp "$src" "$dst"
            echo "updated $dst"
        fi
    done
done
if [ $check -eq 1 ] && [ $status -eq 0 ]; then
    echo ".github: in sync with $*"
fi
exit $status
