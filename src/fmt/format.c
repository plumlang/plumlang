/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The printer behind `plum fmt` (see format.h for the style).
 *
 * It walks the syntax tree and writes lines. The tokens are kept beside
 * the tree, for what the tree leaves out: where brackets were and which
 * parentheses the source wrote. Literals are copied from the source.
 *
 * Comments are merged in by position. Before each statement (or item,
 * or closing brace) the comments that come before it in the source are
 * written on lines of their own. When a line ends, the comments that
 * were passed on the way (inside an expression put on one line) and the
 * one after the code on the source line just finished go at its end.
 */
#include "fmt/format.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/array.h"
#include "util/intern.h"

#include <stdint.h>
#include <string.h>

#define NO_TOKEN SIZE_MAX

/* A line of output: code in Fmt.out, and a trailing comment in Fmt.notes. */
typedef struct Line {
    size_t start;
    size_t end;
    size_t note; /* the comment's offset in Fmt.notes */
    size_t note_len;
} Line;

typedef PLUM_ARRAY(char) CharBuf;

typedef struct Fmt {
    const char *src;
    size_t src_len;
    const PlumToken *tokens;
    size_t token_count;
    size_t *partner; /* for each '(' token, its ')'; NO_TOKEN otherwise */
    PLUM_ARRAY(uint32_t) line_starts;

    const PlumComment *comments;
    size_t comment_count;
    bool *done;   /* each comment, once written */
    size_t next;  /* no comment before this one is left */
    uint32_t pos; /* how far into the source the output has got */

    CharBuf out;
    CharBuf notes;
    PLUM_ARRAY(Line) lines;
    size_t line_start; /* where the line being written starts in out */
    bool line_open;    /* something is on it (its indentation, at least) */
    int indent;
} Fmt;

/* ------------------------------------------------------------------ */
/* Source positions and tokens                                         */
/* ------------------------------------------------------------------ */

/* The 1-based line of a byte offset. */
static uint32_t line_of(const Fmt *f, uint32_t offset) {
    size_t lo = 0;
    size_t hi = f->line_starts.len;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->line_starts.items[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (uint32_t)(lo + 1);
}

static uint32_t span_end(PlumSpan s) {
    return s.offset + s.length;
}

/* The first token that starts at or after `offset`. */
static size_t token_at(const Fmt *f, uint32_t offset) {
    size_t lo = 0;
    size_t hi = f->token_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->tokens[mid].span.offset < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The last token that starts before `end`. */
static size_t token_before(const Fmt *f, uint32_t end) {
    size_t i = token_at(f, end);
    return i > 0 ? i - 1 : 0;
}

static uint32_t token_end(const Fmt *f, size_t i) {
    return span_end(f->tokens[i].span);
}

static uint32_t first_line(const Fmt *f, size_t tok) {
    return f->tokens[tok].span.line;
}

/* The line a token ends on (a """ string can take several). */
static uint32_t last_line(const Fmt *f, size_t tok) {
    PlumSpan s = f->tokens[tok].span;
    return s.length > 0 ? line_of(f, s.offset + s.length - 1) : s.line;
}

/* ------------------------------------------------------------------ */
/* Where an expression is in the tokens, with its parentheses          */
/* ------------------------------------------------------------------ */

static size_t outer_first(const Fmt *f, const PlumExpr *e);
static size_t outer_last(const Fmt *f, const PlumExpr *e);

/* The first token of `e` itself, inside any parentheses around it. A
 * node's span starts at its leftmost child's span, which leaves out the
 * child's own parentheses. */
static size_t inner_first(const Fmt *f, const PlumExpr *e) {
    switch (e->kind) {
    case PLUM_EXPR_BINARY:
        return outer_first(f, e->as.binary.left);
    case PLUM_EXPR_RANGE:
        return outer_first(f, e->as.range.low);
    case PLUM_EXPR_IS:
    case PLUM_EXPR_AS:
        return outer_first(f, e->as.cast.value);
    case PLUM_EXPR_CALL:
        return outer_first(f, e->as.call.callee);
    case PLUM_EXPR_INDEX:
        return outer_first(f, e->as.index.object);
    case PLUM_EXPR_FIELD:
        return outer_first(f, e->as.field.object);
    default:
        return token_at(f, e->span.offset);
    }
}

static size_t inner_last(const Fmt *f, const PlumExpr *e) {
    switch (e->kind) {
    case PLUM_EXPR_BINARY:
        return outer_last(f, e->as.binary.right);
    case PLUM_EXPR_RANGE:
        return outer_last(f, e->as.range.high);
    case PLUM_EXPR_UNARY:
        return outer_last(f, e->as.unary.operand);
    case PLUM_EXPR_TRY:
    case PLUM_EXPR_SPAWN:
        return outer_last(f, e->as.inner);
    default:
        return token_before(f, span_end(e->span));
    }
}

/* Widen [*first, *last] over the parentheses written around it; returns
 * how many pairs there are. */
static int wrap(const Fmt *f, size_t *first, size_t *last) {
    int layers = 0;
    while (*first > 0 && *last + 1 < f->token_count && f->partner[*first - 1] == *last + 1) {
        (*first)--;
        (*last)++;
        layers++;
    }
    return layers;
}

static size_t outer_first(const Fmt *f, const PlumExpr *e) {
    size_t first = inner_first(f, e);
    size_t last = inner_last(f, e);
    wrap(f, &first, &last);
    return first;
}

static size_t outer_last(const Fmt *f, const PlumExpr *e) {
    size_t first = inner_first(f, e);
    size_t last = inner_last(f, e);
    wrap(f, &first, &last);
    return last;
}

/*
 * The tokens of `e` with its parentheses, less `skip` pairs that belong
 * to the parent (the parentheses of a call with one argument). Returns
 * whether any pair is left.
 */
static bool extent(const Fmt *f, const PlumExpr *e, int skip, size_t *first, size_t *last) {
    *first = inner_first(f, e);
    *last = inner_last(f, e);
    int layers = wrap(f, first, last);
    while (skip > 0 && layers > 0) {
        (*first)++;
        (*last)--;
        skip--;
        layers--;
    }
    return layers > 0;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

static void put_n(Fmt *f, const char *s, size_t n) {
    if (!f->line_open) {
        for (int i = 0; i < f->indent * 4; i++) {
            plum_array_push(&f->out, ' ');
        }
        f->line_open = true;
    }
    plum_array_reserve(&f->out, n);
    memcpy(f->out.items + f->out.len, s, n);
    f->out.len += n;
}

static void put(Fmt *f, const char *s) {
    put_n(f, s, strlen(s));
}

/* Copy source text as it is. */
static void put_source(Fmt *f, PlumSpan span) {
    put_n(f, f->src + span.offset, span.length);
}

/* The output has got this far into the source. */
static void reach(Fmt *f, uint32_t offset) {
    if (offset > f->pos) {
        f->pos = offset;
    }
}

/* A comment's text, without trailing spaces (or the \r of a CRLF line). */
static size_t comment_len(const Fmt *f, const PlumComment *c) {
    size_t n = c->span.length;
    while (n > 1) {
        char ch = f->src[c->span.offset + n - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r') {
            break;
        }
        n--;
    }
    return n;
}

/* The first comment not yet written, or comment_count. */
static size_t next_comment(Fmt *f) {
    while (f->next < f->comment_count && f->done[f->next]) {
        f->next++;
    }
    return f->next;
}

static bool last_line_blank(const Fmt *f) {
    return f->lines.len > 0 && plum_array_last(&f->lines).start == plum_array_last(&f->lines).end;
}

static void push_line(Fmt *f, size_t note, size_t note_len) {
    Line line = {f->line_start, f->out.len, note, note_len};
    plum_array_push(&f->lines, line);
    f->line_start = f->out.len;
    f->line_open = false;
}

/* A blank line, unless there is one already or nothing yet. */
static void blank_line(Fmt *f) {
    if (f->lines.len > 0 && !last_line_blank(f)) {
        push_line(f, 0, 0);
    }
}

/* Comment `i` on a line of its own. */
static void comment_line(Fmt *f, size_t i) {
    const PlumComment *c = &f->comments[i];
    put_n(f, f->src + c->span.offset, comment_len(f, c));
    push_line(f, 0, 0);
    f->done[i] = true;
}

/*
 * End the line being written. The comments passed on the way, and one
 * after the code on the source line where the output has got to, go at
 * its end (several are joined into one); not that one unless
 * `take_after`, when what comes next on that source line should have it.
 */
static void end_line_after(Fmt *f, bool take_after) {
    uint32_t line = f->pos > 0 && take_after ? line_of(f, f->pos - 1) : 0;
    size_t note = f->notes.len;
    size_t i;
    while ((i = next_comment(f)) < f->comment_count) {
        const PlumComment *c = &f->comments[i];
        bool passed = c->span.offset < f->pos;
        bool after = !c->own_line && line != 0 && c->span.line == line;
        if (!passed && !after) {
            break;
        }
        if (!f->line_open) {
            comment_line(f, i);
            continue;
        }
        if (f->notes.len > note) {
            plum_array_push(&f->notes, ' ');
        }
        size_t n = comment_len(f, c);
        plum_array_reserve(&f->notes, n);
        memcpy(f->notes.items + f->notes.len, f->src + c->span.offset, n);
        f->notes.len += n;
        f->done[i] = true;
    }
    if (f->line_open) {
        push_line(f, note, f->notes.len - note);
    }
}

static void end_line(Fmt *f) {
    end_line_after(f, true);
}

/*
 * The comments before `limit` in the source, each on its own line, with
 * the blank lines the source has between them. `*prev_line` is the
 * source line of what was written last in this sequence (0: nothing).
 */
static void leading_comments(Fmt *f, uint32_t limit, uint32_t *prev_line) {
    size_t i;
    while ((i = next_comment(f)) < f->comment_count && f->comments[i].span.offset < limit) {
        uint32_t line = f->comments[i].span.line;
        if (*prev_line != 0 && line > *prev_line + 1) {
            blank_line(f);
        }
        comment_line(f, i);
        *prev_line = line;
    }
}

/* Before the next statement or item of a sequence, starting at `offset`:
 * its comments, and a blank line if the source has one before it. */
static void before_item(Fmt *f, uint32_t offset, uint32_t *prev_line) {
    leading_comments(f, offset, prev_line);
    uint32_t line = line_of(f, offset);
    if (*prev_line != 0 && line > *prev_line + 1) {
        blank_line(f);
    }
}

/* Are there comments not yet written before `offset`? */
static bool comments_before(Fmt *f, uint32_t offset) {
    size_t i = next_comment(f);
    return i < f->comment_count && f->comments[i].span.offset < offset;
}

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

static void type(Fmt *f, const PlumType *t) {
    switch (t->kind) {
    case PLUM_TYPE_NAME:
        if (t->as.name.module != NULL) {
            put(f, t->as.name.module);
            put(f, ".");
        }
        put(f, t->as.name.name);
        if (t->as.name.arg_count > 0) {
            put(f, "[");
            for (size_t i = 0; i < t->as.name.arg_count; i++) {
                if (i > 0) {
                    put(f, ", ");
                }
                type(f, t->as.name.args[i]);
            }
            put(f, "]");
        }
        break;
    case PLUM_TYPE_OPTIONAL:
        type(f, t->as.optional);
        put(f, "?");
        break;
    case PLUM_TYPE_FUNC:
        put(f, "(");
        for (size_t i = 0; i < t->as.func.param_count; i++) {
            if (i > 0) {
                put(f, ", ");
            }
            type(f, t->as.func.params[i]);
        }
        put(f, ") -> ");
        if (t->as.func.result != NULL) {
            type(f, t->as.func.result);
        } else {
            put(f, "none");
        }
        break;
    case PLUM_TYPE_ERROR:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Bracketed lists: one line, or one item per line                     */
/* ------------------------------------------------------------------ */

typedef enum ItemKind {
    ITEM_EXPR,  /* list items, PlumExpr * */
    ITEM_ENTRY, /* map entries */
    ITEM_ARG,   /* call arguments */
    ITEM_PARAM, /* parameters */
    ITEM_CASE,  /* kind cases */
} ItemKind;

typedef struct Items {
    ItemKind kind;
    const void *items;
    size_t count;
} Items;

static void expr(Fmt *f, const PlumExpr *e);
static void expr_skip(Fmt *f, const PlumExpr *e, int skip);
static void stmt(Fmt *f, const PlumStmt *s);
static void block(Fmt *f, const PlumStmt *b);
static void items(Fmt *f, const Items *it, size_t open, size_t close, bool padded, bool commas);

/* The parentheses of a call with one positional argument wrap it. */
static int arg_skip(const Items *it, const PlumArg *arg) {
    return it->count == 1 && arg->name == NULL ? 1 : 0;
}

/* The first and last token of item `i`. */
static void item_tokens(const Fmt *f, const Items *it, size_t i, size_t *first, size_t *last) {
    switch (it->kind) {
    case ITEM_EXPR:
        extent(f, ((PlumExpr *const *)it->items)[i], 0, first, last);
        break;
    case ITEM_ENTRY: {
        const PlumMapEntryExpr *entry = &((const PlumMapEntryExpr *)it->items)[i];
        size_t ignored;
        extent(f, entry->key, 0, first, &ignored);
        extent(f, entry->value, 0, &ignored, last);
        break;
    }
    case ITEM_ARG: {
        const PlumArg *arg = &((const PlumArg *)it->items)[i];
        extent(f, arg->value, arg_skip(it, arg), first, last);
        if (arg->name != NULL) {
            *first = token_at(f, arg->span.offset);
        }
        break;
    }
    case ITEM_PARAM: {
        const PlumParam *param = &((const PlumParam *)it->items)[i];
        *first = token_at(f, param->span.offset);
        *last = token_before(f, span_end(param->span));
        break;
    }
    case ITEM_CASE: {
        const PlumCase *c = &((const PlumCase *)it->items)[i];
        *first = token_at(f, c->span.offset);
        *last = token_before(f, span_end(c->span));
        break;
    }
    }
}

static void params(Fmt *f, const PlumParam *ps, size_t count, size_t open);

static void item(Fmt *f, const Items *it, size_t i) {
    switch (it->kind) {
    case ITEM_EXPR:
        expr(f, ((PlumExpr *const *)it->items)[i]);
        break;
    case ITEM_ENTRY: {
        const PlumMapEntryExpr *entry = &((const PlumMapEntryExpr *)it->items)[i];
        expr(f, entry->key);
        put(f, ": ");
        expr(f, entry->value);
        break;
    }
    case ITEM_ARG: {
        const PlumArg *arg = &((const PlumArg *)it->items)[i];
        if (arg->name != NULL) {
            put(f, arg->name);
            put(f, ": ");
        }
        expr_skip(f, arg->value, arg_skip(it, arg));
        break;
    }
    case ITEM_PARAM: {
        const PlumParam *param = &((const PlumParam *)it->items)[i];
        put(f, param->name);
        if (param->type != NULL) {
            put(f, ": ");
            type(f, param->type);
        }
        if (param->fallback != NULL) {
            put(f, " = ");
            expr(f, param->fallback);
        }
        break;
    }
    case ITEM_CASE: {
        const PlumCase *c = &((const PlumCase *)it->items)[i];
        put(f, c->name);
        if (c->param_count > 0) {
            size_t open = token_at(f, c->span.offset) + 1;
            params(f, c->params, c->param_count, open);
        }
        break;
    }
    }
}

/* Does the source break the list: a newline after the opening bracket,
 * between two items, or before the closing one? */
static bool broken(const Fmt *f, const Items *it, size_t open, size_t close) {
    uint32_t line = last_line(f, open);
    for (size_t i = 0; i < it->count; i++) {
        size_t first;
        size_t last;
        item_tokens(f, it, i, &first, &last);
        if (first_line(f, first) != line) {
            return true;
        }
        line = last_line(f, last);
    }
    return it->count > 0 && first_line(f, close) != line;
}

/*
 * The items between bracket tokens `open` and `close`, brackets included:
 * `[a, b]`, or `{ a, b }` when `padded`. Spread over lines in the source,
 * they go one per line, each followed by a comma when `commas`.
 */
static void items(Fmt *f, const Items *it, size_t open, size_t close, bool padded, bool commas) {
    const char *lb = plum_token_kind_text(f->tokens[open].kind);
    const char *rb = plum_token_kind_text(f->tokens[close].kind);
    if (!broken(f, it, open, close)) {
        put(f, lb);
        if (padded && it->count > 0) {
            put(f, " ");
        }
        for (size_t i = 0; i < it->count; i++) {
            if (i > 0) {
                put(f, ", ");
            }
            item(f, it, i);
        }
        if (padded && it->count > 0) {
            put(f, " ");
        }
        put(f, rb);
        reach(f, token_end(f, close));
        return;
    }
    put(f, lb);
    reach(f, token_end(f, open));
    /* A comment after the first item on the bracket's line is the item's. */
    size_t first0;
    size_t last0;
    item_tokens(f, it, 0, &first0, &last0);
    end_line_after(f, first_line(f, first0) != last_line(f, open));
    f->indent++;
    uint32_t prev = 0;
    for (size_t i = 0; i < it->count; i++) {
        size_t first;
        size_t last;
        item_tokens(f, it, i, &first, &last);
        before_item(f, f->tokens[first].span.offset, &prev);
        item(f, it, i);
        if (commas) {
            put(f, ",");
        }
        reach(f, token_end(f, last));
        end_line(f);
        prev = last_line(f, last);
    }
    leading_comments(f, f->tokens[close].span.offset, &prev);
    f->indent--;
    put(f, rb);
    reach(f, token_end(f, close));
}

/* Parameters from the '(' token `open`. */
static void params(Fmt *f, const PlumParam *ps, size_t count, size_t open) {
    Items it = {ITEM_PARAM, ps, count};
    size_t close = f->partner[open];
    items(f, &it, open, close == NO_TOKEN ? open + 1 : close, false, true);
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

/* An expression that is copied from the source: a literal. Comments in
 * it (in a string interpolation) come along. */
static void verbatim(Fmt *f, PlumSpan span) {
    put_source(f, span);
    for (size_t i = next_comment(f); i < f->comment_count; i++) {
        uint32_t at = f->comments[i].span.offset;
        if (at >= span_end(span)) {
            break;
        }
        if (at >= span.offset) {
            f->done[i] = true;
        }
    }
}

/* `{ a }` of an if expression; `tall`: on lines of its own. */
static void if_branch(Fmt *f, const PlumExpr *e, bool tall) {
    if (!tall) {
        put(f, "{ ");
        expr(f, e);
        put(f, " }");
        return;
    }
    size_t first;
    size_t last;
    extent(f, e, 0, &first, &last);
    put(f, "{");
    end_line(f);
    f->indent++;
    uint32_t prev = 0;
    before_item(f, f->tokens[first].span.offset, &prev);
    expr(f, e);
    end_line(f);
    prev = last_line(f, last);
    /* comments up to the '}' after it */
    size_t close = last + 1;
    while (close < f->token_count && f->tokens[close].kind != PLUM_TOK_RBRACE) {
        close++;
    }
    if (close < f->token_count) {
        leading_comments(f, f->tokens[close].span.offset, &prev);
    }
    f->indent--;
    put(f, "}");
}

/*
 * if c { a } else { b }, on one line; or, if the source spreads it over
 * several (`tall`), each branch on lines of its own:
 *
 *     plant x = if c {
 *         a
 *     } else {
 *         b
 *     }
 */
static void if_expr(Fmt *f, const PlumExpr *e, bool tall) {
    put(f, "if ");
    expr(f, e->as.if_.cond);
    put(f, " ");
    if_branch(f, e->as.if_.then, tall);
    put(f, " else ");
    const PlumExpr *otherwise = e->as.if_.otherwise;
    size_t first;
    size_t last;
    if (otherwise->kind == PLUM_EXPR_IF && !extent(f, otherwise, 0, &first, &last)) {
        if_expr(f, otherwise, tall); /* else if */
        reach(f, token_end(f, last));
        return;
    }
    if_branch(f, otherwise, tall);
}

static void expr_bare(Fmt *f, const PlumExpr *e) {
    switch (e->kind) {
    case PLUM_EXPR_INT:
    case PLUM_EXPR_FLOAT:
    case PLUM_EXPR_STRING:
    case PLUM_EXPR_INTERP:
        verbatim(f, e->span);
        break;
    case PLUM_EXPR_BOOL: /* good, or true: the spec allows both */
        verbatim(f, e->span);
        break;
    case PLUM_EXPR_NONE:
        put(f, "none");
        break;
    case PLUM_EXPR_NAME:
        put(f, e->as.name);
        break;
    case PLUM_EXPR_SELF:
        put(f, "self");
        break;
    case PLUM_EXPR_SUPER:
        put(f, "super");
        break;
    case PLUM_EXPR_WILDCARD:
        put(f, "_");
        break;
    case PLUM_EXPR_LIST: {
        Items it = {ITEM_EXPR, e->as.list.items, e->as.list.count};
        items(f, &it, token_at(f, e->span.offset), token_before(f, span_end(e->span)), false, true);
        break;
    }
    case PLUM_EXPR_MAP: {
        if (e->as.map.count == 0) {
            put(f, "{:}");
            break;
        }
        Items it = {ITEM_ENTRY, e->as.map.entries, e->as.map.count};
        items(f, &it, token_at(f, e->span.offset), token_before(f, span_end(e->span)), false, true);
        break;
    }
    case PLUM_EXPR_UNARY: {
        const PlumExpr *operand = e->as.unary.operand;
        put(f, plum_token_kind_text(e->as.unary.op));
        if (e->as.unary.op == PLUM_TOK_NOT) {
            put(f, " ");
        } else if (operand->kind == PLUM_EXPR_UNARY && operand->as.unary.op == e->as.unary.op) {
            size_t first;
            size_t last;
            if (!extent(f, operand, 0, &first, &last)) {
                put(f, " "); /* - -x, not --x */
            }
        }
        expr(f, operand);
        break;
    }
    case PLUM_EXPR_BINARY:
        expr(f, e->as.binary.left);
        put(f, " ");
        put(f, plum_token_kind_text(e->as.binary.op));
        put(f, " ");
        expr(f, e->as.binary.right);
        break;
    case PLUM_EXPR_RANGE:
        expr(f, e->as.range.low);
        put(f, e->as.range.inclusive ? "..=" : "..");
        expr(f, e->as.range.high);
        break;
    case PLUM_EXPR_IS:
    case PLUM_EXPR_AS:
        expr(f, e->as.cast.value);
        put(f, e->kind == PLUM_EXPR_IS ? " is " : " as ");
        type(f, e->as.cast.type);
        break;
    case PLUM_EXPR_CALL: {
        expr(f, e->as.call.callee);
        Items it = {ITEM_ARG, e->as.call.args, e->as.call.arg_count};
        items(f, &it, outer_last(f, e->as.call.callee) + 1, token_before(f, span_end(e->span)),
              false, true);
        break;
    }
    case PLUM_EXPR_INDEX:
        expr(f, e->as.index.object);
        put(f, "[");
        expr(f, e->as.index.index);
        put(f, "]");
        break;
    case PLUM_EXPR_FIELD:
        expr(f, e->as.field.object);
        put(f, e->as.field.optional ? "?." : ".");
        put(f, e->as.field.name);
        break;
    case PLUM_EXPR_LAMBDA:
        params(f, e->as.lambda.params, e->as.lambda.param_count, token_at(f, e->span.offset));
        put(f, " => ");
        if (e->as.lambda.block != NULL) {
            block(f, e->as.lambda.block);
        } else {
            expr(f, e->as.lambda.body);
        }
        break;
    case PLUM_EXPR_IF: {
        size_t first = inner_first(f, e);
        size_t last = inner_last(f, e);
        if_expr(f, e, first_line(f, first) != last_line(f, last));
        break;
    }
    case PLUM_EXPR_TRY:
        put(f, "try ");
        expr(f, e->as.inner);
        break;
    case PLUM_EXPR_SPAWN:
        put(f, "spawn ");
        expr(f, e->as.inner);
        break;
    case PLUM_EXPR_MATCH: {
        put(f, "match ");
        expr(f, e->as.match.subject);
        size_t open = outer_last(f, e->as.match.subject) + 1;
        uint32_t close = span_end(e->span) - 1;
        if (e->as.match.arm_count == 0 && !comments_before(f, close)) {
            put(f, " {}");
            break;
        }
        put(f, " {");
        reach(f, token_end(f, open));
        end_line(f);
        f->indent++;
        uint32_t prev = 0;
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            const PlumMatchArm *arm = &e->as.match.arms[i];
            before_item(f, arm->span.offset, &prev);
            if (arm->pattern_count == 0) {
                put(f, "else");
            }
            for (size_t j = 0; j < arm->pattern_count; j++) {
                if (j > 0) {
                    put(f, ", ");
                }
                expr(f, arm->patterns[j]);
            }
            put(f, " => ");
            if (arm->body->kind == PLUM_STMT_BLOCK) {
                block(f, arm->body);
            } else {
                stmt(f, arm->body);
            }
            reach(f, span_end(arm->span));
            end_line(f);
            prev = line_of(f, span_end(arm->span) - 1);
        }
        leading_comments(f, close, &prev);
        f->indent--;
        put(f, "}");
        break;
    }
    case PLUM_EXPR_ERROR:
        break;
    }
}

/* An expression, with one pair of the parentheses the source put around
 * it (less `skip` pairs that belong to the parent). */
static void expr_skip(Fmt *f, const PlumExpr *e, int skip) {
    size_t first;
    size_t last;
    bool parens = extent(f, e, skip, &first, &last);
    if (parens) {
        put(f, "(");
    }
    expr_bare(f, e);
    if (parens) {
        put(f, ")");
    }
    reach(f, token_end(f, last));
}

static void expr(Fmt *f, const PlumExpr *e) {
    expr_skip(f, e, 0);
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

/* A statement's first line in the source. */
static uint32_t stmt_offset(const Fmt *f, const PlumStmt *s) {
    if (s->kind == PLUM_STMT_EXPR) {
        return f->tokens[outer_first(f, s->as.expr)].span.offset;
    }
    if (s->kind == PLUM_STMT_ASSIGN) {
        return f->tokens[outer_first(f, s->as.assign.target)].span.offset;
    }
    return s->span.offset;
}

/* `{ statements }`: `{` here, the statements on their own lines, `}`. */
static void block(Fmt *f, const PlumStmt *b) {
    uint32_t close = span_end(b->span) - 1;
    if (b->as.block.count == 0 && !comments_before(f, close)) {
        put(f, "{}");
        reach(f, span_end(b->span));
        return;
    }
    put(f, "{");
    reach(f, b->span.offset + 1);
    end_line(f);
    f->indent++;
    uint32_t prev = 0;
    for (size_t i = 0; i < b->as.block.count; i++) {
        const PlumStmt *s = b->as.block.stmts[i];
        before_item(f, stmt_offset(f, s), &prev);
        stmt(f, s);
        end_line(f);
        prev = line_of(f, span_end(s->span) - 1);
    }
    leading_comments(f, close, &prev);
    f->indent--;
    put(f, "}");
    reach(f, span_end(b->span));
}

static void branch(Fmt *f, const PlumBranch *b) {
    put(f, "branch ");
    put(f, b->name);
    params(f, b->params, b->param_count, token_at(f, b->span.offset) + 2);
    if (b->result != NULL) {
        put(f, " -> ");
        type(f, b->result);
    }
    if (b->arrow != NULL) {
        put(f, " => ");
        expr(f, b->arrow);
    } else if (b->body != NULL) {
        put(f, " ");
        block(f, b->body);
    }
}

/* A tree's or trait's fields and methods, in source order. */
typedef struct Member {
    uint32_t offset;
    uint32_t end;
    const PlumField *field;
    const PlumBranch *method;
} Member;

static void field(Fmt *f, const PlumField *fd) {
    put(f, fd->name);
    put(f, ": ");
    if (fd->weak) {
        put(f, "weak ");
    }
    type(f, fd->type);
    if (fd->fallback != NULL) {
        put(f, " = ");
        expr(f, fd->fallback);
    }
}

static void tree(Fmt *f, const PlumStmt *s) {
    put(f, s->kind == PLUM_STMT_TRAIT ? "trait " : "tree ");
    put(f, s->as.tree.name);
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        put(f, i == 0 ? " : " : ", ");
        type(f, s->as.tree.supers[i]);
    }
    size_t open = token_at(f, s->span.offset);
    while (open < f->token_count && f->tokens[open].kind != PLUM_TOK_LBRACE) {
        open++;
    }
    uint32_t close = span_end(s->span) - 1;

    size_t count = s->as.tree.field_count + s->as.tree.method_count;
    if (count == 0 && !comments_before(f, close)) {
        put(f, " {}");
        return;
    }
    Member *members = plum_alloc_zeroed(count > 0 ? count : 1, sizeof *members);
    size_t n = 0;
    for (size_t i = 0; i < s->as.tree.field_count; i++) {
        const PlumField *fd = &s->as.tree.fields[i];
        Member m = {fd->span.offset, span_end(fd->span), fd, NULL};
        members[n++] = m;
    }
    for (size_t i = 0; i < s->as.tree.method_count; i++) {
        const PlumBranch *b = &s->as.tree.methods[i];
        Member m = {b->span.offset, span_end(b->span), NULL, b};
        size_t at = n++;
        while (at > 0 && members[at - 1].offset > m.offset) {
            members[at] = members[at - 1];
            at--;
        }
        members[at] = m;
    }

    put(f, " {");
    reach(f, token_end(f, open));
    end_line(f);
    f->indent++;
    uint32_t prev = 0;
    for (size_t i = 0; i < n; i++) {
        before_item(f, members[i].offset, &prev);
        if (members[i].field != NULL) {
            field(f, members[i].field);
        } else {
            branch(f, members[i].method);
        }
        reach(f, members[i].end);
        end_line(f);
        prev = line_of(f, members[i].end - 1);
    }
    leading_comments(f, close, &prev);
    f->indent--;
    put(f, "}");
    plum_free(members);
}

static void kind(Fmt *f, const PlumStmt *s) {
    put(f, "kind ");
    put(f, s->as.kind.name);
    put(f, " ");
    size_t open = token_at(f, s->span.offset);
    while (open < f->token_count && f->tokens[open].kind != PLUM_TOK_LBRACE) {
        open++;
    }
    Items it = {ITEM_CASE, s->as.kind.cases, s->as.kind.case_count};
    items(f, &it, open, token_before(f, span_end(s->span)), true, false);
}

static void seed(Fmt *f, const PlumStmt *s) {
    put(f, "seed ");
    put(f, s->as.seed.path);
    if (s->as.seed.alias != NULL) {
        put(f, " as ");
        put(f, s->as.seed.alias);
    }
    if (s->as.seed.names != NULL) {
        put(f, " { ");
        for (size_t i = 0; i < s->as.seed.name_count; i++) {
            if (i > 0) {
                put(f, ", ");
            }
            put(f, s->as.seed.names[i]);
        }
        put(f, " }");
    }
}

static void if_stmt(Fmt *f, const PlumStmt *s) {
    put(f, "if ");
    if (s->kind == PLUM_STMT_IF_PLANT) {
        put(f, "plant ");
        put(f, s->as.if_.name);
        put(f, " = ");
        expr(f, s->as.if_.value);
    } else {
        expr(f, s->as.if_.cond);
    }
    put(f, " ");
    block(f, s->as.if_.then);
    const PlumStmt *otherwise = s->as.if_.otherwise;
    if (otherwise != NULL) {
        put(f, " else ");
        if (otherwise->kind == PLUM_STMT_BLOCK) {
            block(f, otherwise);
        } else {
            if_stmt(f, otherwise);
        }
    }
}

static void stmt(Fmt *f, const PlumStmt *s) {
    switch (s->kind) {
    case PLUM_STMT_EXPR:
        expr(f, s->as.expr);
        break;
    case PLUM_STMT_DECL:
        put(f, s->as.decl.constant ? "root " : "plant ");
        put(f, s->as.decl.name);
        if (s->as.decl.type != NULL) {
            put(f, ": ");
            type(f, s->as.decl.type);
        }
        if (s->as.decl.value != NULL) {
            put(f, " = ");
            expr(f, s->as.decl.value);
        }
        break;
    case PLUM_STMT_ASSIGN:
        expr(f, s->as.assign.target);
        put(f, " ");
        put(f, plum_token_kind_text(s->as.assign.op));
        put(f, " ");
        expr(f, s->as.assign.value);
        break;
    case PLUM_STMT_BLOCK:
        block(f, s);
        break;
    case PLUM_STMT_IF:
    case PLUM_STMT_IF_PLANT:
        if_stmt(f, s);
        break;
    case PLUM_STMT_WHILE:
        put(f, "while ");
        expr(f, s->as.while_.cond);
        put(f, " ");
        block(f, s->as.while_.body);
        break;
    case PLUM_STMT_FOR:
        put(f, "for ");
        put(f, s->as.for_.first);
        if (s->as.for_.second != NULL) {
            put(f, ", ");
            put(f, s->as.for_.second);
        }
        put(f, " in ");
        expr(f, s->as.for_.iterable);
        put(f, " ");
        block(f, s->as.for_.body);
        break;
    case PLUM_STMT_BREAK:
        put(f, "break");
        break;
    case PLUM_STMT_CONTINUE:
        put(f, "continue");
        break;
    case PLUM_STMT_RETURN:
        put(f, "return");
        if (s->as.expr != NULL) {
            put(f, " ");
            expr(f, s->as.expr);
        }
        break;
    case PLUM_STMT_THROW:
        put(f, "throw ");
        expr(f, s->as.expr);
        break;
    case PLUM_STMT_TRY:
        put(f, "try ");
        block(f, s->as.try_.body);
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            const PlumCatch *c = &s->as.try_.catches[i];
            put(f, " catch ");
            put(f, c->name);
            if (c->type != NULL) {
                put(f, ": ");
                type(f, c->type);
            }
            put(f, " ");
            block(f, c->body);
        }
        if (s->as.try_.finally != NULL) {
            put(f, " finally ");
            block(f, s->as.try_.finally);
        }
        break;
    case PLUM_STMT_BRANCH:
        branch(f, &s->as.branch);
        break;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
        tree(f, s);
        break;
    case PLUM_STMT_KIND:
        kind(f, s);
        break;
    case PLUM_STMT_SEED:
        seed(f, s);
        break;
    case PLUM_STMT_ERROR:
        break;
    }
    reach(f, span_end(s->span));
}

static bool is_declaration(const PlumStmt *s) {
    return s->kind == PLUM_STMT_BRANCH || s->kind == PLUM_STMT_TREE || s->kind == PLUM_STMT_TRAIT ||
           s->kind == PLUM_STMT_KIND;
}

/* A blank line before line `at`, unless there is one around it. */
static void insert_blank(Fmt *f, size_t at) {
    if (at == 0 || f->lines.items[at - 1].start == f->lines.items[at - 1].end ||
        (at < f->lines.len && f->lines.items[at].start == f->lines.items[at].end)) {
        return;
    }
    plum_array_reserve(&f->lines, 1);
    memmove(&f->lines.items[at + 1], &f->lines.items[at], (f->lines.len - at) * sizeof(Line));
    Line blank = {0, 0, 0, 0};
    f->lines.items[at] = blank;
    f->lines.len++;
}

static void program(Fmt *f, const PlumProgram *p) {
    uint32_t prev = 0;
    const PlumStmt *prev_stmt = NULL;
    bool prev_long = false;
    for (size_t i = 0; i < p->count; i++) {
        const PlumStmt *s = p->stmts[i];
        size_t group = f->lines.len; /* where its comments start */
        before_item(f, stmt_offset(f, s), &prev);
        size_t first = f->lines.len;
        stmt(f, s);
        end_line(f);
        bool is_long = f->lines.len - first > 1;
        /* Functions, trees, traits and kinds that take more than a line
         * have a blank line on each side. */
        if (prev_stmt != NULL &&
            ((is_declaration(prev_stmt) && prev_long) || (is_declaration(s) && is_long))) {
            insert_blank(f, group);
        }
        prev = line_of(f, span_end(s->span) - 1);
        prev_stmt = s;
        prev_long = is_long;
    }
    leading_comments(f, UINT32_MAX, &prev);
}

/* ------------------------------------------------------------------ */
/* Putting the lines together                                          */
/* ------------------------------------------------------------------ */

/* The width of a line's last row, in characters. */
static size_t width(const Fmt *f, const Line *line) {
    size_t from = line->start;
    for (size_t i = line->start; i < line->end; i++) {
        if (f->out.items[i] == '\n') {
            from = i + 1;
        }
    }
    size_t w = 0;
    for (size_t i = from; i < line->end; i++) {
        if (((unsigned char)f->out.items[i] & 0xC0) != 0x80) {
            w++;
        }
    }
    return w;
}

static void emit(CharBuf *text, const char *s, size_t n) {
    plum_array_reserve(text, n);
    memcpy(text->items + text->len, s, n);
    text->len += n;
}

/* The lines as text. Trailing comments on neighbouring lines line up, two
 * spaces after the longest line's code. */
static char *assemble(Fmt *f, size_t *len) {
    while (last_line_blank(f)) {
        f->lines.len--;
    }
    CharBuf text = {0};
    for (size_t i = 0; i < f->lines.len;) {
        size_t run = i;
        size_t column = 0;
        while (run < f->lines.len && f->lines.items[run].note_len > 0) {
            const Line *line = &f->lines.items[run];
            /* a line with a """ string over several rows stands alone */
            bool rows = memchr(f->out.items + line->start, '\n', line->end - line->start) != NULL;
            if (rows && run > i) {
                break;
            }
            size_t w = width(f, line);
            column = w > column ? w : column;
            run++;
            if (rows) {
                break;
            }
        }
        if (run == i) {
            run = i + 1; /* no comment: the line alone */
        }
        for (; i < run; i++) {
            const Line *line = &f->lines.items[i];
            emit(&text, f->out.items + line->start, line->end - line->start);
            if (line->note_len > 0) {
                for (size_t w = width(f, line); w < column + 2; w++) {
                    plum_array_push(&text, ' ');
                }
                emit(&text, f->notes.items + line->note, line->note_len);
            }
            plum_array_push(&text, '\n');
        }
    }
    plum_array_push(&text, '\0');
    *len = text.len - 1;
    return text.items;
}

/* ------------------------------------------------------------------ */
/* The whole file                                                      */
/* ------------------------------------------------------------------ */

char *plum_format(const PlumSource *source, PlumDiagList *diags, size_t *len) {
    PlumArena arena;
    PlumInterner names;
    PlumTokenList tokens = {0};
    PlumCommentList comments = {0};
    plum_arena_init(&arena);
    plum_interner_init(&names);
    plum_lex_comments(source, &arena, &names, diags, &tokens, &comments);
    PlumParser parser;
    plum_parser_init(&parser, &tokens, &arena, &names, diags);
    PlumProgram prog = plum_parse_program(&parser);

    char *result = NULL;
    if (diags->errors == 0) {
        Fmt f;
        memset(&f, 0, sizeof f);
        f.src = source->text;
        f.src_len = source->length;
        f.tokens = tokens.items;
        f.token_count = tokens.len;
        f.comments = comments.items;
        f.comment_count = comments.len;
        f.done = plum_alloc_zeroed(comments.len + 1, sizeof(bool));
        f.partner = plum_alloc_zeroed(tokens.len + 1, sizeof(size_t));
        PLUM_ARRAY(size_t) stack = {0};
        for (size_t i = 0; i < tokens.len; i++) {
            f.partner[i] = NO_TOKEN;
            if (tokens.items[i].kind == PLUM_TOK_LPAREN) {
                plum_array_push(&stack, i);
            } else if (tokens.items[i].kind == PLUM_TOK_RPAREN && stack.len > 0) {
                f.partner[plum_array_pop(&stack)] = i;
            }
        }
        plum_array_free(&stack);
        plum_array_push(&f.line_starts, 0);
        for (size_t i = 0; i < source->length; i++) {
            if (source->text[i] == '\n') {
                plum_array_push(&f.line_starts, (uint32_t)(i + 1));
            }
        }

        program(&f, &prog);
        result = assemble(&f, len);

        plum_free(f.done);
        plum_free(f.partner);
        plum_array_free(&f.line_starts);
        plum_array_free(&f.out);
        plum_array_free(&f.notes);
        plum_array_free(&f.lines);
    }
    plum_array_free(&comments);
    plum_array_free(&tokens);
    plum_interner_free(&names);
    plum_arena_free(&arena);
    return result;
}
