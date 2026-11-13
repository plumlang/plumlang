#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Print the instruction table of docs/bytecode.md from the one list of
# opcodes in src/compiler/opcodes.h, so the two can't disagree.
#
# Usage: sh tools/bytecode-table.sh [path/to/opcodes.h]
header=${1:-src/compiler/opcodes.h}
echo "| # | Instruction | Operands | What it does |"
echo "|---|---|---|---|"
sed -n '/^#define PLUM_OPCODES(X)/,/^$/p' "$header" |
    sed -n 's/.*X(\([A-Z_0-9]*\), \([0-9]\), "\(.*\)").*/\1|\2|\3/p' |
    awk -F'|' '{ printf "| %d | `%s` | %s | %s |\n", NR - 1, $1, $2, $3 }'
