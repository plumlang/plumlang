#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Print the CHANGELOG.md section for a version (the release notes):
#
#   sh tools/release/notes.sh 1.2.3
set -eu
[ $# -eq 1 ] || { echo "usage: notes.sh <version>" >&2; exit 2; }
cd "$(dirname "$0")/../.."
awk -v v="v$1" '
    /^## / { if (found) exit; if ($2 == v) { found = 1; next } }
    found { print }
    END { if (!found) { print "CHANGELOG.md has no section for " v > "/dev/stderr"; exit 1 } }
' CHANGELOG.md
