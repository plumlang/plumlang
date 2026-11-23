/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* Fuzz target: any bytes, as a source file, through the lexer. */
#include "fuzz.h"

#include "lexer/lexer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_text(data, size);
    PlumSource source = {"fuzz.plum", text, size};
    PlumArena arena;
    PlumInterner names;
    PlumDiagList diags;
    PlumTokenList tokens = {0};
    plum_arena_init(&arena);
    plum_interner_init(&names);
    plum_diag_init(&diags, &source);
    plum_lex(&source, &arena, &names, &diags, &tokens);
    /* What the lexer promises (lexer.h): the list ends with EOF. */
    if (tokens.len == 0 || tokens.items[tokens.len - 1].kind != PLUM_TOK_EOF) {
        abort();
    }
    plum_diag_print(&diags, fuzz_null(), true);
    plum_array_free(&tokens);
    plum_diag_free(&diags);
    plum_interner_free(&names);
    plum_arena_free(&arena);
    free(text);
    return 0;
}
