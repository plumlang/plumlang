/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The parser turns tokens into a syntax tree (spec sections 4 to 9).
 *
 * Expressions are parsed by precedence (Pratt parsing) with the levels of
 * spec section 4.8. On a syntax error the parser reports it once, returns
 * an ERROR node, and stays quiet until it resynchronises at the next
 * statement, so one mistake gives one message.
 */
#ifndef PLUM_PARSER_PARSER_H
#define PLUM_PARSER_PARSER_H

#include "diag/diag.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct PlumParser {
    const PlumToken *tokens;
    size_t count;
    size_t pos;
    PlumArena *arena;
    PlumInterner *names;
    PlumDiagList *diags;
    bool panic; /* an error was reported; stay quiet until resynchronised */
    int depth;  /* how deep the syntax tree being built is (PLUM_MAX_NESTING) */
} PlumParser;

/*
 * How deep a syntax tree may be: brackets, blocks, types and operators
 * inside each other, each operator of a chain like `a + b + c` counting
 * one. Every later stage walks the tree recursively, so this keeps them
 * within the C stack (E0218). No program written by hand comes near it.
 */
#define PLUM_MAX_NESTING 1000

/* `tokens` must end with EOF, as plum_lex leaves them. */
void plum_parser_init(PlumParser *p, const PlumTokenList *tokens, PlumArena *arena,
                      PlumInterner *names, PlumDiagList *diags);

PlumExpr *plum_parse_expr(PlumParser *p);
PlumType *plum_parse_type(PlumParser *p);

/* Parse a whole file. The program lives in the arena. */
PlumProgram plum_parse_program(PlumParser *p);

#endif /* PLUM_PARSER_PARSER_H */
