#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Print Plumlang's keywords, one per line, from the lexer's table in
# src/lexer/lexer.c. tests/highlight/keywords.txt is this output; editor
# grammars check themselves against it (tests/highlight/README.md).
#
# Usage: sh tools/keywords.sh > tests/highlight/keywords.txt
here=$(dirname "$0")
awk '
    /^} keywords\[\] = \{/ { table = 1; next }
    table && /^};/ { exit }
    table && match($0, /\{"[a-z_]+", PLUM_TOK_[A-Z_]+\}/) {
        word = substr($0, RSTART + 2)
        sub(/".*/, "", word)
        print word
    }
' "$here/../src/lexer/lexer.c"
