/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Doc comments (spec section 2.2): the `##` lines directly before a
 * declaration document it, and a file's first `##` lines document the
 * module. Their text is Markdown.
 *
 * The lexer leaves comments out of the tokens; ask it for them with
 * plum_lex_comments and pass them here with the parsed program. The
 * language server's hover uses these; so can `plum doc`.
 */
#ifndef PLUM_PARSER_DOC_H
#define PLUM_PARSER_DOC_H

#include "lexer/lexer.h"
#include "parser/ast.h"

#include <stdbool.h>

/* Can `##` lines document this statement: `branch`, `tree`, `trait`,
 * `kind` or `root`? (Fields, methods and kind cases can be documented
 * too; they are not statements.) */
bool plum_doc_documents(const PlumStmt *s);

/*
 * The Markdown of the `##` lines directly above `at` (the declaration's
 * first token, which starts its line): each line without its `##` and one
 * space after it, joined with '\n'. NULL when there are none. Free the
 * text with plum_free.
 */
char *plum_doc_before(const PlumSource *source, const PlumCommentList *comments, PlumSpan at);

/*
 * The module's doc comment: the file's first run of `##` lines, with only
 * blank lines and other comments before it, unless it is directly before
 * a declaration (and so documents that). NULL when there is none.
 */
char *plum_doc_module(const PlumSource *source, const PlumCommentList *comments,
                      const PlumProgram *program);

#endif /* PLUM_PARSER_DOC_H */
