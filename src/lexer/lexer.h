/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The lexer turns a source file into tokens (spec section 2).
 *
 * Statements end at NEWLINE tokens. A newline produces no token inside
 * ( ) or [ ], inside a string interpolation, after `{`, after a token that
 * cannot end a statement (a binary operator, `,`, `.`, `->`, `=>`, `=`),
 * or after another NEWLINE. The token list always ends with NEWLINE (if
 * there were any tokens) and then EOF.
 *
 * Interpolated strings are split: "a{x}b{y:.2}c" becomes
 * STRING_HEAD(a) IDENT(x) STRING_MID(b) IDENT(y) FORMAT(.2) STRING_TAIL(c).
 *
 * Errors are added to the diagnostics list and lexing carries on, so one
 * run reports every problem; the token list stays usable.
 */
#ifndef PLUM_LEXER_LEXER_H
#define PLUM_LEXER_LEXER_H

#include "diag/diag.h"
#include "lexer/token.h"
#include "util/arena.h"
#include "util/array.h"
#include "util/intern.h"

typedef PLUM_ARRAY(PlumToken) PlumTokenList;

/*
 * Lex `source` into `out`. Decoded string text lives in `arena`; names are
 * interned in `names`; errors go to `diags`.
 */
void plum_lex(const PlumSource *source, PlumArena *arena, PlumInterner *names, PlumDiagList *diags,
              PlumTokenList *out);

#endif /* PLUM_LEXER_LEXER_H */
