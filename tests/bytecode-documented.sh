#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# docs/bytecode.md's instruction table must match src/compiler/opcodes.h.
# Regenerate it with `make bytecode-doc`.
here=$(dirname "$0")
root="$here/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sh "$root/tools/bytecode-table.sh" "$root/src/compiler/opcodes.h" >"$tmp/table"
sed -n '/<!-- opcodes:begin -->/,/<!-- opcodes:end -->/p' "$root/docs/bytecode.md" |
    sed '1d;$d' >"$tmp/doc"
if [ "$(cat "$tmp/table"; printf .)" = "$(cat "$tmp/doc"; printf .)" ]; then
    echo "bytecode: $(($(wc -l <"$tmp/table") - 2)) instructions, documented"
    exit 0
fi
echo "bytecode: docs/bytecode.md is out of date; run 'make bytecode-doc'"
exit 1
