/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "diag/diag.h"

#include "util/alloc.h"

#include <stdarg.h>

void plum_diag_init(PlumDiagList *list, const PlumSource *source) {
    list->source = source;
    list->items.items = NULL;
    list->items.len = 0;
    list->items.cap = 0;
    list->errors = 0;
}

void plum_diag_free(PlumDiagList *list) {
    for (size_t i = 0; i < list->items.len; i++) {
        plum_free(list->items.items[i].message);
    }
    plum_array_free(&list->items);
    list->errors = 0;
}

PLUM_PRINTF(4, 0)
static void add(PlumDiagList *list, PlumSeverity severity, PlumSpan span, const char *fmt,
                va_list args) {
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        plum_fatal("cannot format a diagnostic");
    }
    char *message = plum_alloc((size_t)n + 1);
    vsnprintf(message, (size_t)n + 1, fmt, args);

    PlumDiag diag = {severity, span, message};
    plum_array_push(&list->items, diag);
    if (severity == PLUM_ERROR) {
        list->errors++;
    }
}

void plum_diag_error(PlumDiagList *list, PlumSpan span, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    add(list, PLUM_ERROR, span, fmt, args);
    va_end(args);
}

void plum_diag_warning(PlumDiagList *list, PlumSpan span, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    add(list, PLUM_WARNING, span, fmt, args);
    va_end(args);
}

void plum_diag_print(const PlumDiagList *list, FILE *out) {
    for (size_t i = 0; i < list->items.len; i++) {
        const PlumDiag *d = &list->items.items[i];
        fprintf(out, "%s:%u:%u: %s: %s\n", list->source->path, (unsigned)d->span.line,
                (unsigned)d->span.column, d->severity == PLUM_ERROR ? "error" : "warning",
                d->message);
    }
}
