/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * `plum lsp`: a Language Server Protocol server on standard input and
 * output (docs/lsp.md). Messages are JSON-RPC 2.0, each after a
 * `Content-Length` header. The server answers until it is told to exit
 * or its input ends, and never stops on bad input: a message it can't
 * read gets an error response, and the next one is read.
 */
#ifndef PLUM_LSP_LSP_H
#define PLUM_LSP_LSP_H

#include <stdio.h>

/* Serve `in` and `out` until `exit` (or the end of the input). Returns
 * the exit code: 0 after a `shutdown` request, 1 otherwise. */
int plum_lsp_serve(FILE *in, FILE *out);

/* The `plum lsp` command. */
int plum_lsp_command(int argc, char **argv);

#endif /* PLUM_LSP_LSP_H */
