/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* fileno and isatty are POSIX, hidden by -std=c11 unless asked for. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "diag/diag.h"

#include "util/alloc.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

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

/* Diagnostics come from several stages; print them in source order. */
static int by_position(const void *a, const void *b) {
    const PlumDiag *x = *(const PlumDiag *const *)a;
    const PlumDiag *y = *(const PlumDiag *const *)b;
    if (x->span.offset != y->span.offset) {
        return x->span.offset < y->span.offset ? -1 : 1;
    }
    return x < y ? -1 : x > y; /* keep the order they were reported in */
}

/* At most this many diagnostics are printed; the rest are counted. */
#define PLUM_DIAG_PRINT_LIMIT 50

typedef struct Style {
    const char *error, *warning, *gutter, *bold, *reset;
} Style;

static const Style plain = {"", "", "", "", ""};
static const Style colored = {"\x1b[1;31m", "\x1b[1;33m", "\x1b[34m", "\x1b[1m", "\x1b[0m"};

static unsigned digits(unsigned n) {
    unsigned d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

/* Length of the valid UTF-8 character at s[0..n), or 0 if it isn't one. */
static size_t valid_char(const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    size_t len = u[0] < 0x80             ? 1
                 : (u[0] & 0xE0) == 0xC0 ? 2
                 : (u[0] & 0xF0) == 0xE0 ? 3
                 : (u[0] & 0xF8) == 0xF0 ? 4
                                         : 0;
    if (len == 0 || len > n) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return len;
}

/*
 * Bytes taken by one displayed character: a valid one, or a single bad
 * byte (shown as U+FFFD). The line and its underline step the same way,
 * so the underline stays aligned.
 */
static size_t step(const char *s, size_t n) {
    size_t len = valid_char(s, n);
    return len == 0 ? 1 : len;
}

/*
 * Print the source line of `d` with an underline under its span:
 *
 *    3 | plant x
 *      |        ^
 */
static void print_snippet(const PlumSource *src, const PlumDiag *d, unsigned width, const Style *st,
                          FILE *out) {
    const char *text = src->text;
    size_t offset = d->span.offset < src->length ? d->span.offset : src->length;

    size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n') {
        line_start--;
    }
    size_t line_end = offset;
    while (line_end < src->length && text[line_end] != '\n') {
        line_end++;
    }
    size_t shown_end = line_end;
    if (shown_end > line_start && text[shown_end - 1] == '\r') {
        shown_end--;
    }
    /* A span at the very end of the file (or of an empty last line): show
     * the line it follows. */
    if (line_start == shown_end && line_start > 0 && offset == src->length) {
        line_end = line_start - 1;
        line_start = line_end;
        while (line_start > 0 && text[line_start - 1] != '\n') {
            line_start--;
        }
        shown_end = line_end;
        offset = line_end;
    }

    unsigned line_no = d->span.line;
    if (offset != d->span.offset) {
        /* We moved to the previous line; count its number. */
        line_no = 1;
        for (size_t i = 0; i < line_start; i++) {
            line_no += text[i] == '\n';
        }
    }

    fprintf(out, "%s%*u |%s ", st->gutter, (int)width, line_no, st->reset);
    for (size_t i = line_start; i < shown_end;) {
        size_t n = valid_char(text + i, shown_end - i);
        if (n == 0) {
            fputs("\xEF\xBF\xBD", out); /* U+FFFD for each invalid byte */
            i++;
        } else {
            fwrite(text + i, 1, n, out);
            i += n;
        }
    }
    fputc('\n', out);

    /* Under every character before the span, a space (a tab under a tab,
     * so the caret lines up whatever the tab width). */
    fprintf(out, "%s%*s |%s ", st->gutter, (int)width, "", st->reset);
    for (size_t i = line_start; i < offset && i < shown_end; i += step(text + i, shown_end - i)) {
        fputc(text[i] == '\t' ? '\t' : ' ', out);
    }
    size_t span_end = d->span.offset + d->span.length;
    if (span_end > shown_end) {
        span_end = shown_end; /* underline only the first line of a long span */
    }
    size_t marks = 0;
    for (size_t i = offset; i < span_end; i += step(text + i, span_end - i)) {
        marks++;
    }
    fputs(d->severity == PLUM_ERROR ? st->error : st->warning, out);
    fputc('^', out);
    for (size_t i = 1; i < marks; i++) {
        fputc('~', out);
    }
    fprintf(out, "%s\n", st->reset);
}

void plum_diag_print(const PlumDiagList *list, FILE *out, bool color) {
    const Style *st = color ? &colored : &plain;
    size_t n = list->items.len;
    if (n == 0) {
        return;
    }
    const PlumDiag **sorted = plum_alloc_zeroed(n, sizeof *sorted);
    unsigned max_line = 1;
    for (size_t i = 0; i < n; i++) {
        sorted[i] = &list->items.items[i];
        if (sorted[i]->span.line > max_line) {
            max_line = sorted[i]->span.line;
        }
    }
    qsort(sorted, n, sizeof *sorted, by_position);
    unsigned width = digits(max_line);

    size_t shown = n < PLUM_DIAG_PRINT_LIMIT ? n : PLUM_DIAG_PRINT_LIMIT;
    size_t warnings = n - list->errors;
    for (size_t i = 0; i < shown; i++) {
        const PlumDiag *d = sorted[i];
        bool is_error = d->severity == PLUM_ERROR;
        fprintf(out, "%s%s:%u:%u:%s %s%s:%s %s%s%s\n", st->bold, list->source->path,
                (unsigned)d->span.line, (unsigned)d->span.column, st->reset,
                is_error ? st->error : st->warning, is_error ? "error" : "warning", st->reset,
                st->bold, d->message, st->reset);
        print_snippet(list->source, d, width, st, out);
    }
    if (n > shown) {
        fprintf(out, "... and %zu more\n", n - shown);
    }

    /* "2 errors", "1 error and 1 warning" */
    if (list->errors > 0) {
        fprintf(out, "%s%zu error%s%s", st->error, list->errors, list->errors == 1 ? "" : "s",
                st->reset);
        if (warnings > 0) {
            fprintf(out, " and %zu warning%s", warnings, warnings == 1 ? "" : "s");
        }
    } else {
        fprintf(out, "%s%zu warning%s%s", st->warning, warnings, warnings == 1 ? "" : "s",
                st->reset);
    }
    fputc('\n', out);
    plum_free(sorted);
}

#if defined(_WIN32)
#include <io.h>
#define plum_isatty(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define plum_isatty(f) isatty(fileno(f))
#endif

bool plum_diag_use_color(FILE *out) {
    const char *force = getenv("FORCE_COLOR");
    if (force != NULL && force[0] != '\0' && strcmp(force, "0") != 0) {
        return true;
    }
    const char *no = getenv("NO_COLOR");
    if (no != NULL && no[0] != '\0') {
        return false;
    }
    return plum_isatty(out) != 0;
}
