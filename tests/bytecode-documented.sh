#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# docs/bytecode.md's instruction table must match src/compiler/opcodes.h.
# Regenerate it with `make bytecode-doc`. Every instruction must also be
# emitted by the compiler and run by the VM, and every instruction family
# docs/bytecode.md explains in its text (GET_FIELD_L, FIELD_ADD_INT_K,
# CALL_METHOD, ...) must still exist.
here=$(dirname "$0")
root="$here/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sh "$root/tools/bytecode-table.sh" "$root/src/compiler/opcodes.h" >"$tmp/table"
sed -n '/<!-- opcodes:begin -->/,/<!-- opcodes:end -->/p' "$root/docs/bytecode.md" |
    sed '1d;$d' >"$tmp/doc"
if [ "$(cat "$tmp/table"; printf .)" != "$(cat "$tmp/doc"; printf .)" ]; then
    echo "bytecode: docs/bytecode.md is out of date; run 'make bytecode-doc'"
    exit 1
fi
sed -n '/^#define PLUM_OPCODES(X)/,/^$/p' "$root/src/compiler/opcodes.h" |
    sed -n 's/.*X(\([A-Z_0-9]*\), [0-9], .*/\1/p' >"$tmp/names"
status=0
while read -r op; do
    if ! cat "$root"/src/compiler/*.c | grep -q "PLUM_OP_$op\([^A-Z_0-9]\|$\)"; then
        echo "bytecode: the compiler never emits $op"
        status=1
    fi
    if ! grep -q "case PLUM_OP_$op:" "$root/src/vm/vm.c"; then
        echo "bytecode: the VM has no case for $op"
        status=1
    fi
done <"$tmp/names"
# Every instruction that reads a local slot (its description names a
# local) is one compiler/moves.c knows: else a move could empty a slot it
# still reads.
sed -n '/^#define PLUM_OPCODES(X)/,/^$/p' "$root/src/compiler/opcodes.h" | tr -d '\\' |
    tr '\n' ' ' | grep -o 'X([A-Z_0-9]*, [0-9], *"[^"]*"' |
    sed -n 's/X(\([A-Z_0-9]*\), .*[Ll]ocal.*/\1/p' >"$tmp/locals"
sed -n '/^static void slot_uses/,/^}/p' "$root/src/compiler/moves.c" >"$tmp/uses"
while read -r op; do
    if ! grep -q "case PLUM_OP_$op:" "$tmp/uses"; then
        echo "bytecode: $op reads a local, but compiler/moves.c's slot_uses doesn't list it"
        status=1
    fi
done <"$tmp/locals"
# Instructions named in the text, outside the generated table.
sed '/<!-- opcodes:begin -->/,/<!-- opcodes:end -->/d' "$root/docs/bytecode.md" |
    grep -o '`[A-Z][A-Z_0-9]*`' | tr -d '`' | sort -u >"$tmp/named"
while read -r op; do
    case "$op" in
    _* | *_) continue ;; # a family's suffix or prefix (`_K`, `LOCAL_`)
    esac
    if ! grep -qx "$op" "$tmp/names"; then
        echo "bytecode: docs/bytecode.md names $op, which is not an instruction"
        status=1
    fi
done <"$tmp/named"
if [ $status -eq 0 ]; then
    echo "bytecode: $(($(wc -l <"$tmp/table") - 2)) instructions, documented, emitted and run"
fi
exit $status
