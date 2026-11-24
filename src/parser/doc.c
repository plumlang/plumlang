/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "parser/doc.h"

#include "util/alloc.h"
#include "util/array.h"

#include <string.h>

typedef PLUM_ARRAY(char) Text;

bool plum_doc_documents(const PlumStmt *s) {
    switch (s->kind) {
    case PLUM_STMT_BRANCH:
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
    case PLUM_STMT_KIND:
        return true;
    case PLUM_STMT_DECL:
        return s->as.decl.constant;
    default:
        return false;
    }
}

static uint32_t span_end(PlumSpan s) {
    return s.offset + s.length;
}

/* Is source[from, to) spaces with exactly one line break? */
static bool next_line(const PlumSource *source, uint32_t from, uint32_t to) {
    if (from > to || to > source->length) {
        return false;
    }
    int breaks = 0;
    for (uint32_t i = from; i < to; i++) {
        char c = source->text[i];
        if (c == '\n') {
            breaks++;
        } else if (c != ' ' && c != '\t' && c != '\r') {
            return false;
        }
    }
    return breaks == 1;
}

static bool is_doc(const PlumComment *c) {
    return c->kind == PLUM_COMMENT_DOC && c->own_line;
}

/* The Markdown of comments[first..last], a run of `##` lines. */
static char *doc_text(const PlumSource *source, const PlumCommentList *comments, size_t first,
                      size_t last) {
    Text t = {0};
    for (size_t i = first; i <= last; i++) {
        const PlumComment *c = &comments->items[i];
        const char *s = source->text + c->span.offset + 2; /* after "##" */
        size_t n = c->span.length - 2;
        if (n > 0 && *s == ' ') {
            s++, n--;
        }
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
            n--;
        }
        if (i > first) {
            plum_array_push(&t, '\n');
        }
        plum_array_reserve(&t, n + 1);
        memcpy(t.items + t.len, s, n);
        t.len += n;
    }
    plum_array_push(&t, '\0');
    return t.items;
}

/* The first comment of the run of `##` lines ending with comments[last]. */
static size_t run_start(const PlumComment *items, size_t last) {
    size_t first = last;
    while (first > 0 && is_doc(&items[first - 1]) &&
           items[first - 1].span.line + 1 == items[first].span.line) {
        first--;
    }
    return first;
}

char *plum_doc_before(const PlumSource *source, const PlumCommentList *comments, PlumSpan at) {
    /* the last comment before `at` */
    size_t n = 0;
    while (n < comments->len && comments->items[n].span.offset < at.offset) {
        n++;
    }
    if (n == 0) {
        return NULL;
    }
    size_t last = n - 1;
    const PlumComment *c = &comments->items[last];
    if (!is_doc(c) || !next_line(source, span_end(c->span), at.offset)) {
        return NULL;
    }
    return doc_text(source, comments, run_start(comments->items, last), last);
}

char *plum_doc_module(const PlumSource *source, const PlumCommentList *comments,
                      const PlumProgram *program) {
    const PlumStmt *first_stmt = program->count > 0 ? program->stmts[0] : NULL;
    uint32_t code = first_stmt != NULL ? first_stmt->span.offset : (uint32_t)source->length;
    size_t first = 0;
    while (first < comments->len && comments->items[first].span.offset < code &&
           !is_doc(&comments->items[first])) {
        if (!comments->items[first].own_line) {
            return NULL;
        }
        first++;
    }
    if (first == comments->len || comments->items[first].span.offset >= code) {
        return NULL;
    }
    size_t last = first;
    while (last + 1 < comments->len && is_doc(&comments->items[last + 1]) &&
           comments->items[last + 1].span.line == comments->items[last].span.line + 1) {
        last++;
    }
    if (first_stmt != NULL && plum_doc_documents(first_stmt) &&
        next_line(source, span_end(comments->items[last].span), code)) {
        return NULL; /* it documents the declaration */
    }
    return doc_text(source, comments, first, last);
}
