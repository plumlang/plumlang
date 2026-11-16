/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Positions and file names as the language server protocol has them.
 *
 * A position is a line and a character, both from 0, where characters are
 * counted in UTF-16 code units (the protocol's default): a character
 * outside the Basic Multilingual Plane counts two, an invalid byte one.
 * Lines end at "\n", "\r\n" or "\r". Files are named by URIs: file URIs
 * are turned into paths and back; other schemes (an editor's unsaved
 * "untitled:" buffers) are used as they are.
 */
#ifndef PLUM_LSP_TEXT_H
#define PLUM_LSP_TEXT_H

#include <stddef.h>

/* Where each line of a text starts. */
typedef struct PlumLspLines {
    const char *text;
    size_t len;
    size_t *starts; /* byte offset of each line */
    size_t count;
} PlumLspLines;

void plum_lsp_lines_init(PlumLspLines *lines, const char *text, size_t len);
void plum_lsp_lines_free(PlumLspLines *lines);

/* The byte offset of a position; one past the end of its line, or of the
 * text, is clamped. */
size_t plum_lsp_offset(const PlumLspLines *lines, long long line, long long character);
/* The position of a byte offset (clamped to the text). */
void plum_lsp_position(const PlumLspLines *lines, size_t offset, long long *line,
                       long long *character);

/* The path a URI names (malloc'd): file:///a%20b/x.plum is "/a b/x.plum",
 * file:///c%3A/x.plum is "c:/x.plum"; any other URI is returned as it is. */
char *plum_lsp_uri_to_path(const char *uri, size_t len);
/* The file URI for a path (malloc'd). */
char *plum_lsp_path_to_uri(const char *path);

#endif /* PLUM_LSP_TEXT_H */
