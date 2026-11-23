/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * What the language server knows about one open file: the file and the
 * modules it imports, lexed, parsed, resolved and type-checked (never
 * compiled or run), and the questions an editor asks about it.
 *
 * The checks run as `plum check` runs them, into each module's
 * diagnostics, so the editor shows the same errors with the same codes
 * and messages. Where `plum check` stops at the first module with errors,
 * the server carries on into a scratch list it throws away: the names in
 * a half-typed file still resolve, and its types are still known when
 * nothing is unresolved, for hover, definition and completion.
 *
 * No table is kept for the editor while checking: the resolver already
 * links each name in the syntax tree to its symbol (which knows where it
 * is declared), and the type checker gives each expression and symbol its
 * type. The questions are answered by walking the checked tree, so
 * checking for `plum run` costs exactly what it did.
 */
#ifndef PLUM_LSP_ANALYSIS_H
#define PLUM_LSP_ANALYSIS_H

#include "lsp/json.h"
#include "module/loader.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct PlumLspAnalysis {
    PlumModules modules;
    PlumModule **order; /* imports first, the file itself last */
    size_t count;
    PlumModule *main;
} PlumLspAnalysis;

/*
 * Check the file `path` with the text `text` (copied). Imported files are
 * read with `read` (NULL: from disk).
 */
PlumLspAnalysis *plum_lsp_analyze(const char *path, const char *text, size_t len,
                                  PlumModuleReader read, void *read_ctx);
void plum_lsp_analysis_free(PlumLspAnalysis *a);

/* Does the analysis include the file `path` (the file itself or an import)? */
bool plum_lsp_analysis_uses(const PlumLspAnalysis *a, const char *path);

/* How the answers name files: the URI of a module's path (malloc'd), or
 * NULL for one with none (the standard library's own modules). */
typedef struct PlumLspNamer {
    char *(*uri)(void *ctx, const char *path);
    void *ctx;
} PlumLspNamer;

/* Write a module's diagnostics as an array of LSP Diagnostic objects. */
void plum_lsp_write_diagnostics(PlumJsonWriter *w, const PlumModule *m);

/* The answers, written as the result of the request (null when none). */
void plum_lsp_hover(PlumJsonWriter *w, const PlumLspAnalysis *a, size_t offset);
void plum_lsp_definition(PlumJsonWriter *w, const PlumLspAnalysis *a, size_t offset,
                         const PlumLspNamer *namer);
void plum_lsp_document_symbols(PlumJsonWriter *w, const PlumLspAnalysis *a);
/* Completion needs the file's text again, to check it with the name being
 * typed in place of what is there. */
void plum_lsp_completion(PlumJsonWriter *w, const PlumLspAnalysis *a, const char *text, size_t len,
                         size_t offset, PlumModuleReader read, void *read_ctx);

#endif /* PLUM_LSP_ANALYSIS_H */
