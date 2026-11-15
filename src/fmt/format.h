/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * `plum fmt`: one canonical layout for Plumlang source (spec section 14).
 *
 * The formatter parses a file and prints the syntax tree back out:
 *
 *   - four spaces per level; `{` ends its line, `}` starts its own
 *   - spaces around binary operators and after commas; none around `..`
 *   - at most one blank line between statements, none at the start or
 *     end of a block, and one around top-level functions, trees, traits
 *     and kinds that take more than a line
 *   - a list, map, call, parameter list or kind that the source spreads
 *     over several lines gets one item per line (with a trailing comma,
 *     except for kind cases); otherwise it stays on one line
 *   - literals keep their source spelling (0xFF, 1_000, escapes, """),
 *     and parentheses the source wrote are kept
 *
 * Comments come from the lexer's side list. One on a line of its own
 * stays on a line of its own, before the statement, item or closing
 * brace that follows it; one after code stays after that code. Trailing
 * comments on neighbouring lines are lined up.
 *
 * Only a file without syntax errors is formatted, so its meaning is
 * unchanged: formatting again gives the same text, and `plum ast` of
 * the result is the same as of the original.
 */
#ifndef PLUM_FMT_FORMAT_H
#define PLUM_FMT_FORMAT_H

#include "diag/diag.h"

#include <stddef.h>

/*
 * Format `source`. Returns the new text (NUL-terminated; free it with
 * plum_free) and its length in *len, or NULL if the file has syntax
 * errors, which are then in `diags`.
 */
char *plum_format(const PlumSource *source, PlumDiagList *diags, size_t *len);

/*
 * The `plum fmt` command: `plum fmt [--check] [--stdout] <path|->...`.
 * Returns the exit code.
 */
int plum_fmt_command(int argc, char **argv);

#endif /* PLUM_FMT_FORMAT_H */
