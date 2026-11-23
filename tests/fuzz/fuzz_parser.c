/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* Fuzz target: any bytes, as a source file, through the lexer and the
 * parser, and the syntax tree printed (`plum ast`). */
#include "fuzz.h"

#include "lexer/lexer.h"
#include "parser/parser.h"

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
    PlumParser parser;
    plum_parser_init(&parser, &tokens, &arena, &names, &diags);
    PlumProgram program = plum_parse_program(&parser);
    plum_print_program(fuzz_null(), &program);
    plum_diag_print(&diags, fuzz_null(), false);
    plum_array_free(&tokens);
    plum_diag_free(&diags);
    plum_interner_free(&names);
    plum_arena_free(&arena);
    free(text);
    return 0;
}
