/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Diagnostics: errors and warnings found while compiling, with where they
 * are. Every stage appends to one PlumDiagList; the CLI prints them. Rich
 * rendering (the source line, an underline, hints) arrives with #11.
 */
#ifndef PLUM_DIAG_DIAG_H
#define PLUM_DIAG_DIAG_H

#include "util/array.h"

#include <stdint.h>
#include <stdio.h>

/* A source file held in memory. */
typedef struct PlumSource {
    const char *path; /* as given on the command line */
    const char *text; /* NUL-terminated */
    size_t length;
} PlumSource;

/* A place in a source file: byte offset and length, 1-based line and column. */
typedef struct PlumSpan {
    uint32_t offset;
    uint32_t length;
    uint32_t line;
    uint32_t column; /* in characters, not bytes */
} PlumSpan;

typedef enum PlumSeverity {
    PLUM_ERROR,
    PLUM_WARNING,
} PlumSeverity;

typedef struct PlumDiag {
    PlumSeverity severity;
    PlumSpan span;
    char *message; /* owned */
} PlumDiag;

typedef struct PlumDiagList {
    const PlumSource *source;
    PLUM_ARRAY(PlumDiag) items;
    size_t errors;
} PlumDiagList;

void plum_diag_init(PlumDiagList *list, const PlumSource *source);
void plum_diag_free(PlumDiagList *list);

#if defined(__GNUC__) || defined(__clang__)
#define PLUM_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define PLUM_PRINTF(fmt, args)
#endif

void plum_diag_error(PlumDiagList *list, PlumSpan span, const char *fmt, ...) PLUM_PRINTF(3, 4);
void plum_diag_warning(PlumDiagList *list, PlumSpan span, const char *fmt, ...) PLUM_PRINTF(3, 4);

/* Print every diagnostic as "path:line:column: error: message". */
void plum_diag_print(const PlumDiagList *list, FILE *out);

#endif /* PLUM_DIAG_DIAG_H */
