/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "lsp/analysis.h"

#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"
#include "check/types.h"
#include "lexer/lexer.h"
#include "lsp/text.h"
#include "parser/doc.h"
#include "stdlib/native.h"
#include "util/alloc.h"
#include "util/array.h"
#include "vm/methods.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Checking                                                            */
/* ------------------------------------------------------------------ */

/*
 * The modules in order, as `plum check` does: resolve, type-check and
 * check the flow of each, stopping at the first with errors. After that
 * point (or with syntax errors anywhere), resolve and type-check into a
 * scratch list instead, for the editor's questions only. A module is
 * type-checked only when it resolved cleanly and everything before it
 * was type-checked, so the checker never meets a name without a symbol.
 */
static void check(PlumLspAnalysis *a) {
    bool ok = !plum_modules_failed(&a->modules);
    bool typed = true;
    for (size_t i = 0; i < a->count; i++) {
        PlumModule *m = a->order[i];
        PlumDiagList scratch;
        plum_diag_init(&scratch, &m->source);
        PlumDiagList *d = ok ? &m->diags : &scratch;
        size_t before = d->errors;
        plum_resolve(&m->program, &a->modules.arena, &a->modules.names, d, &m->exports);
        bool can_type = typed && d->errors == before;
        if (can_type) {
            plum_typecheck(&m->program, &a->modules.arena, &a->modules.names, d);
        }
        typed = can_type;
        if (ok) {
            if (m->diags.errors == 0) {
                plum_check_flow(&m->program, &m->diags);
            }
            ok = m->diags.errors == 0;
        }
        plum_diag_free(&scratch);
    }
}

PlumLspAnalysis *plum_lsp_analyze(const char *path, const char *text, size_t len,
                                  PlumModuleReader read, void *read_ctx) {
    PlumLspAnalysis *a = plum_alloc_zeroed(1, sizeof *a);
    plum_modules_init(&a->modules);
    a->modules.read = read;
    a->modules.read_ctx = read_ctx;
    char *copy = malloc(len + 1); /* the loader frees it with free() */
    if (copy == NULL) {
        plum_fatal("out of memory");
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    a->main = plum_modules_add(&a->modules, path, copy, len, &a->order, &a->count);
    check(a);
    return a;
}

void plum_lsp_analysis_free(PlumLspAnalysis *a) {
    if (a == NULL) {
        return;
    }
    plum_free(a->order);
    plum_modules_free(&a->modules);
    plum_free(a);
}

bool plum_lsp_analysis_uses(const PlumLspAnalysis *a, const char *path) {
    for (size_t i = 0; i < a->count; i++) {
        if (strcmp(a->order[i]->path, path) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Writing positions                                                   */
/* ------------------------------------------------------------------ */

static void write_position(PlumJsonWriter *w, const PlumLspLines *lines, size_t offset) {
    long long line, character;
    plum_lsp_position(lines, offset, &line, &character);
    plum_jw_object(w);
    plum_jw_key(w, "line");
    plum_jw_int(w, line);
    plum_jw_key(w, "character");
    plum_jw_int(w, character);
    plum_jw_object_end(w);
}

static void write_range(PlumJsonWriter *w, const PlumLspLines *lines, size_t start, size_t end) {
    plum_jw_object(w);
    plum_jw_key(w, "start");
    write_position(w, lines, start);
    plum_jw_key(w, "end");
    write_position(w, lines, end < start ? start : end);
    plum_jw_object_end(w);
}

/* The range of a module's text from `start` to `end`. */
static void write_module_range(PlumJsonWriter *w, const PlumModule *m, size_t start, size_t end) {
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, m->text, m->len);
    write_range(w, &lines, start, end);
    plum_lsp_lines_free(&lines);
}

/* Diagnostics in source order, as `plum check` prints them. */
static int by_position(const void *a, const void *b) {
    const PlumDiag *x = *(const PlumDiag *const *)a;
    const PlumDiag *y = *(const PlumDiag *const *)b;
    if (x->span.offset != y->span.offset) {
        return x->span.offset < y->span.offset ? -1 : 1;
    }
    return x < y ? -1 : x > y;
}

static void write_one_diagnostic(PlumJsonWriter *w, const PlumLspLines *lines, const PlumModule *m,
                                 const PlumDiag *d) {
    size_t start = d->span.offset < m->len ? d->span.offset : m->len;
    size_t end = start + d->span.length <= m->len ? start + d->span.length : m->len;
    plum_jw_object(w);
    plum_jw_key(w, "range");
    write_range(w, lines, start, end);
    plum_jw_key(w, "severity");
    plum_jw_int(w, d->severity == PLUM_ERROR ? 1 : 2);
    plum_jw_key(w, "code");
    plum_jw_string(w, d->code);
    plum_jw_key(w, "source");
    plum_jw_string(w, "plum");
    plum_jw_key(w, "message");
    plum_jw_string(w, d->message);
    plum_jw_object_end(w);
}

void plum_lsp_write_diagnostics(PlumJsonWriter *w, const PlumModule *m) {
    size_t n = m->diags.items.len;
    const PlumDiag **sorted = plum_alloc_zeroed(n + 1, sizeof *sorted);
    for (size_t i = 0; i < n; i++) {
        sorted[i] = &m->diags.items.items[i];
    }
    qsort(sorted, n, sizeof *sorted, by_position);
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, m->text, m->len);
    plum_jw_array(w);
    for (size_t i = 0; i < n; i++) {
        write_one_diagnostic(w, &lines, m, sorted[i]);
    }
    plum_jw_array_end(w);
    plum_lsp_lines_free(&lines);
    plum_free(sorted);
}

/* ------------------------------------------------------------------ */
/* Source text                                                         */
/* ------------------------------------------------------------------ */

typedef PLUM_ARRAY(char) Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
    plum_array_reserve(b, n + 1);
    memcpy(b->items + b->len, s, n);
    b->len += n;
    b->items[b->len] = '\0';
}

static void buf_str(Buf *b, const char *s) {
    buf_add(b, s, strlen(s));
}

static const char *buf_text(Buf *b) {
    if (b->items == NULL) {
        buf_add(b, "", 0);
    }
    return b->items;
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           (unsigned char)c >= 0x80;
}

/* The first place `name` stands as a whole word in text[from, to), or -1. */
static long find_word(const char *text, size_t len, size_t from, size_t to, const char *name) {
    size_t n = strlen(name);
    if (to > len) {
        to = len;
    }
    for (size_t i = from; n > 0 && i + n <= to; i++) {
        if (memcmp(text + i, name, n) == 0 && (i == 0 || !is_name_char(text[i - 1])) &&
            (i + n >= len || !is_name_char(text[i + n]))) {
            return (long)i;
        }
    }
    return -1;
}

/* A span's end, clamped to the text. */
static size_t span_end(PlumSpan s, size_t len) {
    size_t end = (size_t)s.offset + s.length;
    return end < len ? end : len;
}

/*
 * The header of a declaration: its text from `from` up to the `{` or `=>`
 * that starts its body, with spaces and line breaks folded and comments
 * left out. "branch area(r: float) -> float", "tree Dog : Animal".
 */
static void header_text(const PlumModule *m, size_t from, size_t to, Buf *out) {
    const char *t = m->text;
    if (to > m->len) {
        to = m->len;
    }
    int depth = 0;
    bool in_string = false;
    bool space = false;
    size_t start_len = out->len;
    for (size_t i = from; i < to && out->len - start_len < 400; i++) {
        char c = t[i];
        if (!in_string) {
            if (c == '#') {
                while (i + 1 < to && t[i + 1] != '\n') {
                    i++;
                }
                space = true;
                continue;
            }
            if (depth == 0 && (c == '{' || (c == '=' && i + 1 < to && t[i + 1] == '>'))) {
                break;
            }
            if (c == '(' || c == '[') {
                depth++;
            } else if ((c == ')' || c == ']') && depth > 0) {
                depth--;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                space = true;
                continue;
            }
        }
        if (space && out->len > start_len) {
            buf_add(out, " ", 1);
        }
        space = false;
        if (c == '"') {
            in_string = !in_string;
        } else if (in_string && c == '\\' && i + 1 < to) {
            buf_add(out, t + i, 2);
            i++;
            continue;
        }
        buf_add(out, &c, 1);
    }
}

/* The module whose top level has the statement `decl`; `fallback` if none. */
static const PlumModule *module_of(const PlumLspAnalysis *a, const PlumStmt *decl,
                                   const PlumModule *fallback) {
    if (decl == NULL) {
        return fallback;
    }
    for (size_t i = a->count; i-- > 0;) {
        const PlumProgram *p = &a->order[i]->program;
        for (size_t k = 0; k < p->count; k++) {
            if (p->stmts[k] == decl) {
                return a->order[i];
            }
        }
    }
    return fallback;
}

/* The module whose exports `exports` are. */
static const PlumModule *module_with_exports(const PlumLspAnalysis *a, const PlumMap *exports) {
    for (size_t i = 0; i < a->count; i++) {
        if (&a->order[i]->exports == exports) {
            return a->order[i];
        }
    }
    return NULL;
}

static const char *type_text(const PlumTy *t, char *buf, size_t size) {
    if (t == NULL || plum_ty_is_loose(t)) {
        return NULL;
    }
    return plum_ty_text(t, buf, size);
}

/* ------------------------------------------------------------------ */
/* Members of trees                                                    */
/* ------------------------------------------------------------------ */

static const PlumSymbol *parent_tree(const PlumSymbol *tree) {
    if (tree == NULL || tree->decl == NULL || tree->decl->kind != PLUM_STMT_TREE) {
        return NULL;
    }
    const PlumStmt *s = tree->decl;
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        const PlumType *super = s->as.tree.supers[i];
        if (super != NULL && super->kind == PLUM_TYPE_NAME && super->as.name.symbol != NULL &&
            super->as.name.symbol->kind == PLUM_SYM_TREE) {
            return super->as.name.symbol;
        }
    }
    return NULL;
}

/* A tree's field `name`, searching its parents; *owner is the tree declaring it. */
static const PlumField *find_field(const PlumSymbol *tree, const char *name,
                                   const PlumSymbol **owner) {
    int depth = 0;
    for (const PlumSymbol *t = tree; t != NULL && depth < 32; t = parent_tree(t), depth++) {
        if (t->decl == NULL || t->decl->kind != PLUM_STMT_TREE) {
            continue;
        }
        const PlumStmt *s = t->decl;
        for (size_t i = 0; i < s->as.tree.field_count; i++) {
            if (s->as.tree.fields[i].name != NULL && strcmp(s->as.tree.fields[i].name, name) == 0) {
                *owner = t;
                return &s->as.tree.fields[i];
            }
        }
    }
    return NULL;
}

/* A method of a tree or trait, searching its parents and traits. */
static const PlumBranch *find_method(const PlumSymbol *decl, const char *name,
                                     const PlumSymbol **owner, int depth) {
    if (decl == NULL || decl->decl == NULL || depth > 32) {
        return NULL;
    }
    const PlumStmt *s = decl->decl;
    if (s->kind != PLUM_STMT_TREE && s->kind != PLUM_STMT_TRAIT) {
        return NULL;
    }
    for (size_t i = 0; i < s->as.tree.method_count; i++) {
        if (s->as.tree.methods[i].name != NULL && strcmp(s->as.tree.methods[i].name, name) == 0) {
            *owner = decl;
            return &s->as.tree.methods[i];
        }
    }
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        const PlumType *super = s->as.tree.supers[i];
        if (super != NULL && super->kind == PLUM_TYPE_NAME && super->as.name.symbol != NULL) {
            const PlumBranch *m = find_method(super->as.name.symbol, name, owner, depth + 1);
            if (m != NULL) {
                return m;
            }
        }
    }
    return NULL;
}

static const PlumCase *find_case(const PlumSymbol *kind, const char *name) {
    if (kind == NULL || kind->decl == NULL || kind->decl->kind != PLUM_STMT_KIND) {
        return NULL;
    }
    const PlumStmt *s = kind->decl;
    for (size_t i = 0; i < s->as.kind.case_count; i++) {
        if (s->as.kind.cases[i].name != NULL && strcmp(s->as.kind.cases[i].name, name) == 0) {
            return &s->as.kind.cases[i];
        }
    }
    return NULL;
}

/* The tree (or trait) whose members a value of type `t` has, or NULL. */
static const PlumSymbol *tree_of(const PlumTy *t) {
    if (t != NULL && t->kind == PLUM_TY_OPTIONAL) {
        t = t->as.elem;
    }
    if (t != NULL && (t->kind == PLUM_TY_TREE || t->kind == PLUM_TY_TRAIT)) {
        return t->as.decl;
    }
    return NULL;
}

/* The standard module a symbol's `seed` names, or NULL. */
static const PlumNativeModule *native_module_of(const PlumSymbol *sym) {
    if (sym == NULL || sym->decl == NULL || sym->decl->kind != PLUM_STMT_SEED ||
        sym->decl->as.seed.exports != NULL) {
        return NULL;
    }
    const PlumNativeModule *m = plum_native_module(sym->decl->as.seed.path);
    return m != NULL && !plum_native_is_methods(m) ? m : NULL;
}

/* ------------------------------------------------------------------ */
/* Walking the tree for names                                          */
/* ------------------------------------------------------------------ */

typedef enum OccKind {
    OCC_SYMBOL,      /* a name that stands for a symbol: declared or used */
    OCC_MEMBER,      /* the name after the dot of a.b */
    OCC_FIELD_DECL,  /* a field in a tree */
    OCC_METHOD_DECL, /* a method's name in a tree or trait */
    OCC_CASE_DECL,   /* a case in a kind */
    OCC_PARAM_DECL,  /* a parameter of a case: Circle(radius: float) */
    OCC_SEED_PATH,   /* the module a seed names */
    OCC_SEED_NAME,   /* a name a seed imports: seed math { sqrt } */
    OCC_ARG,         /* a keyword argument's name: f(size: 2) */
    OCC_EXPR,        /* an expression, for its type */
} OccKind;

typedef struct Occ {
    OccKind kind;
    size_t start, end;    /* the name, in the file's text */
    const char *name;     /* SYMBOL, MEMBER, SEED_NAME, ARG */
    PlumSymbol *symbol;   /* SYMBOL */
    const PlumExpr *expr; /* MEMBER (the a.b), ARG (the call), EXPR */
    const PlumStmt *stmt; /* *_DECL: the tree or kind; SEED_*: the seed */
    const void *item;     /* FIELD_DECL: PlumField, METHOD_DECL: PlumBranch, CASE_DECL: PlumCase,
                             PARAM_DECL: PlumParam, ARG: PlumArg */
} Occ;

/* The `seed` module named text[0, len) in a program, or NULL. */
static PlumSymbol *find_module(const PlumProgram *p, const char *name, size_t len) {
    for (size_t i = 0; i < p->count; i++) {
        PlumSymbol *sym = p->stmts[i]->symbol;
        if (p->stmts[i]->kind == PLUM_STMT_SEED && sym != NULL && sym->kind == PLUM_SYM_MODULE &&
            strlen(sym->name) == len && memcmp(sym->name, name, len) == 0) {
            return sym;
        }
    }
    return NULL;
}

typedef struct Walk {
    const PlumModule *m;
    void (*visit)(struct Walk *w, const Occ *o);
    void *ctx;
    int depth;
} Walk;

static void walk_expr(Walk *w, const PlumExpr *e);
static void walk_stmt(Walk *w, const PlumStmt *s);

/* Report `name`, found as a word in text[from, to). */
static void visit_name(Walk *w, OccKind kind, const char *name, size_t from, size_t to,
                       PlumSymbol *sym, const PlumStmt *stmt, const void *item) {
    if (name == NULL) {
        return;
    }
    long at = find_word(w->m->text, w->m->len, from, to, name);
    if (at < 0) {
        return;
    }
    Occ o = {kind, (size_t)at, (size_t)at + strlen(name), name, sym, NULL, stmt, item};
    w->visit(w, &o);
}

static void walk_type(Walk *w, const PlumType *t) {
    if (t == NULL || w->depth > 200) {
        return;
    }
    w->depth++;
    switch (t->kind) {
    case PLUM_TYPE_NAME:
        visit_name(w, OCC_SYMBOL, t->as.name.name, t->span.offset, span_end(t->span, w->m->len),
                   t->as.name.symbol, NULL, NULL);
        for (size_t i = 0; i < t->as.name.arg_count; i++) {
            walk_type(w, t->as.name.args[i]);
        }
        break;
    case PLUM_TYPE_OPTIONAL:
        walk_type(w, t->as.optional);
        break;
    case PLUM_TYPE_FUNC:
        for (size_t i = 0; i < t->as.func.param_count; i++) {
            walk_type(w, t->as.func.params[i]);
        }
        walk_type(w, t->as.func.result);
        break;
    case PLUM_TYPE_ERROR:
        break;
    }
    w->depth--;
}

static void walk_params(Walk *w, const PlumParam *params, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const PlumParam *p = &params[i];
        visit_name(w, OCC_SYMBOL, p->name, p->span.offset, span_end(p->span, w->m->len), p->symbol,
                   NULL, NULL);
        walk_type(w, p->type);
        walk_expr(w, p->fallback);
    }
}

static void walk_expr(Walk *w, const PlumExpr *e) {
    if (e == NULL || w->depth > 200) {
        return;
    }
    w->depth++;
    Occ self = {OCC_EXPR, e->span.offset, span_end(e->span, w->m->len), NULL, NULL, e, NULL, NULL};
    w->visit(w, &self);
    switch (e->kind) {
    case PLUM_EXPR_INT:
    case PLUM_EXPR_FLOAT:
    case PLUM_EXPR_STRING:
    case PLUM_EXPR_BOOL:
    case PLUM_EXPR_NONE:
    case PLUM_EXPR_WILDCARD:
    case PLUM_EXPR_ERROR:
    case PLUM_EXPR_SELF:
    case PLUM_EXPR_SUPER:
        break;
    case PLUM_EXPR_NAME: {
        /* `shapes.area` became a name: the name is at its end */
        size_t end = span_end(e->span, w->m->len);
        size_t n = strlen(e->as.name);
        size_t from = end >= n && end - n >= e->span.offset ? end - n : e->span.offset;
        visit_name(w, OCC_SYMBOL, e->as.name, from, end, e->symbol, NULL, NULL);
        /* and `shapes` is the module it came from */
        size_t q = e->span.offset;
        while (q < from && is_name_char(w->m->text[q])) {
            q++;
        }
        PlumSymbol *mod =
            q > e->span.offset && q < from
                ? find_module(&w->m->program, w->m->text + e->span.offset, q - e->span.offset)
                : NULL;
        if (mod != NULL) {
            Occ o = {OCC_SYMBOL, e->span.offset, q, mod->name, mod, NULL, NULL, NULL};
            w->visit(w, &o);
        }
        break;
    }
    case PLUM_EXPR_INTERP:
        for (size_t i = 0; i < e->as.interp.count; i++) {
            walk_expr(w, e->as.interp.parts[i].expr);
        }
        break;
    case PLUM_EXPR_LIST:
        for (size_t i = 0; i < e->as.list.count; i++) {
            walk_expr(w, e->as.list.items[i]);
        }
        break;
    case PLUM_EXPR_MAP:
        for (size_t i = 0; i < e->as.map.count; i++) {
            walk_expr(w, e->as.map.entries[i].key);
            walk_expr(w, e->as.map.entries[i].value);
        }
        break;
    case PLUM_EXPR_UNARY:
        walk_expr(w, e->as.unary.operand);
        break;
    case PLUM_EXPR_BINARY:
        walk_expr(w, e->as.binary.left);
        walk_expr(w, e->as.binary.right);
        break;
    case PLUM_EXPR_RANGE:
        walk_expr(w, e->as.range.low);
        walk_expr(w, e->as.range.high);
        break;
    case PLUM_EXPR_IS:
    case PLUM_EXPR_AS:
        walk_expr(w, e->as.cast.value);
        walk_type(w, e->as.cast.type);
        break;
    case PLUM_EXPR_CALL:
        walk_expr(w, e->as.call.callee);
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            const PlumArg *arg = &e->as.call.args[i];
            if (arg->name != NULL) {
                long at = find_word(w->m->text, w->m->len, arg->span.offset,
                                    span_end(arg->span, w->m->len), arg->name);
                if (at >= 0) {
                    Occ o = {OCC_ARG,   (size_t)at, (size_t)at + strlen(arg->name),
                             arg->name, NULL,       e,
                             NULL,      arg};
                    w->visit(w, &o);
                }
            }
            walk_expr(w, arg->value);
        }
        break;
    case PLUM_EXPR_INDEX:
        walk_expr(w, e->as.index.object);
        walk_expr(w, e->as.index.index);
        break;
    case PLUM_EXPR_FIELD: {
        walk_expr(w, e->as.field.object);
        const PlumExpr *obj = e->as.field.object;
        size_t from = obj != NULL ? span_end(obj->span, w->m->len) : e->span.offset;
        long at = e->as.field.name != NULL
                      ? find_word(w->m->text, w->m->len, from, span_end(e->span, w->m->len),
                                  e->as.field.name)
                      : -1;
        if (at >= 0) {
            Occ o = {OCC_MEMBER,
                     (size_t)at,
                     (size_t)at + strlen(e->as.field.name),
                     e->as.field.name,
                     NULL,
                     e,
                     NULL,
                     NULL};
            w->visit(w, &o);
        }
        break;
    }
    case PLUM_EXPR_LAMBDA:
        walk_params(w, e->as.lambda.params, e->as.lambda.param_count);
        walk_expr(w, e->as.lambda.body);
        walk_stmt(w, e->as.lambda.block);
        break;
    case PLUM_EXPR_IF:
        walk_expr(w, e->as.if_.cond);
        walk_expr(w, e->as.if_.then);
        walk_expr(w, e->as.if_.otherwise);
        break;
    case PLUM_EXPR_TRY:
    case PLUM_EXPR_SPAWN:
        walk_expr(w, e->as.inner);
        break;
    case PLUM_EXPR_MATCH:
        walk_expr(w, e->as.match.subject);
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            const PlumMatchArm *arm = &e->as.match.arms[i];
            for (size_t k = 0; k < arm->pattern_count; k++) {
                walk_expr(w, arm->patterns[k]);
            }
            walk_stmt(w, arm->body);
        }
        break;
    }
    w->depth--;
}

static void walk_branch(Walk *w, const PlumBranch *b, const PlumStmt *tree, PlumSymbol *sym) {
    size_t end = span_end(b->span, w->m->len);
    if (tree != NULL) {
        visit_name(w, OCC_METHOD_DECL, b->name, b->span.offset, end, NULL, tree, b);
    } else {
        visit_name(w, OCC_SYMBOL, b->name, b->span.offset, end, sym, NULL, NULL);
    }
    walk_params(w, b->params, b->param_count);
    walk_type(w, b->result);
    walk_stmt(w, b->body);
    walk_expr(w, b->arrow);
}

/* Where a seed's path is in its text (after `seed`, before any `as` or
 * `{`); false if it isn't there. */
static bool seed_path_at(const PlumModule *m, const PlumStmt *s, size_t *at, size_t *end) {
    size_t stop = span_end(s->span, m->len);
    const char *path = s->as.seed.path;
    size_t n = path != NULL ? strlen(path) : 0;
    for (size_t i = (size_t)s->span.offset + 4; n > 0 && i + n <= stop; i++) {
        if (memcmp(m->text + i, path, n) == 0) {
            *at = i;
            *end = i + n;
            return true;
        }
    }
    return false;
}

static void walk_seed(Walk *w, const PlumStmt *s) {
    size_t start = s->span.offset;
    size_t end = span_end(s->span, w->m->len);
    size_t at = 0, after = 0;
    if (seed_path_at(w->m, s, &at, &after)) {
        Occ o = {OCC_SEED_PATH, at, after, s->as.seed.path, NULL, NULL, s, NULL};
        w->visit(w, &o);
        start = after;
    }
    if (s->as.seed.alias != NULL) {
        visit_name(w, OCC_SYMBOL, s->as.seed.alias, start, end, s->symbol, NULL, NULL);
    }
    for (size_t i = 0; i < s->as.seed.name_count; i++) {
        visit_name(w, OCC_SEED_NAME, s->as.seed.names[i], start, end, NULL, s, NULL);
    }
}

static void walk_stmt(Walk *w, const PlumStmt *s) {
    if (s == NULL || w->depth > 200) {
        return;
    }
    w->depth++;
    size_t start = s->span.offset;
    size_t end = span_end(s->span, w->m->len);
    switch (s->kind) {
    case PLUM_STMT_EXPR:
    case PLUM_STMT_THROW:
    case PLUM_STMT_RETURN:
        walk_expr(w, s->as.expr);
        break;
    case PLUM_STMT_DECL:
        visit_name(w, OCC_SYMBOL, s->as.decl.name, start, end, s->symbol, NULL, NULL);
        walk_type(w, s->as.decl.type);
        walk_expr(w, s->as.decl.value);
        break;
    case PLUM_STMT_ASSIGN:
        walk_expr(w, s->as.assign.target);
        walk_expr(w, s->as.assign.value);
        break;
    case PLUM_STMT_BLOCK:
        for (size_t i = 0; i < s->as.block.count; i++) {
            walk_stmt(w, s->as.block.stmts[i]);
        }
        break;
    case PLUM_STMT_IF:
        walk_expr(w, s->as.if_.cond);
        walk_stmt(w, s->as.if_.then);
        walk_stmt(w, s->as.if_.otherwise);
        break;
    case PLUM_STMT_IF_PLANT:
        visit_name(w, OCC_SYMBOL, s->as.if_.name, start, end, s->symbol, NULL, NULL);
        walk_expr(w, s->as.if_.value);
        walk_stmt(w, s->as.if_.then);
        walk_stmt(w, s->as.if_.otherwise);
        break;
    case PLUM_STMT_WHILE:
        walk_expr(w, s->as.while_.cond);
        walk_stmt(w, s->as.while_.body);
        break;
    case PLUM_STMT_FOR: {
        long first = s->as.for_.first != NULL
                         ? find_word(w->m->text, w->m->len, start, end, s->as.for_.first)
                         : -1;
        visit_name(w, OCC_SYMBOL, s->as.for_.first, start, end, s->symbol, NULL, NULL);
        if (s->as.for_.second != NULL && first >= 0) {
            visit_name(w, OCC_SYMBOL, s->as.for_.second, (size_t)first + 1, end, s->symbol2, NULL,
                       NULL);
        }
        walk_expr(w, s->as.for_.iterable);
        walk_stmt(w, s->as.for_.body);
        break;
    }
    case PLUM_STMT_TRY:
        walk_stmt(w, s->as.try_.body);
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            const PlumCatch *c = &s->as.try_.catches[i];
            visit_name(w, OCC_SYMBOL, c->name, c->span.offset, span_end(c->span, w->m->len),
                       c->symbol, NULL, NULL);
            walk_type(w, c->type);
            walk_stmt(w, c->body);
        }
        walk_stmt(w, s->as.try_.finally);
        break;
    case PLUM_STMT_BRANCH:
        walk_branch(w, &s->as.branch, NULL, s->symbol);
        break;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
        visit_name(w, OCC_SYMBOL, s->as.tree.name, start, end, s->symbol, NULL, NULL);
        for (size_t i = 0; i < s->as.tree.super_count; i++) {
            walk_type(w, s->as.tree.supers[i]);
        }
        for (size_t i = 0; i < s->as.tree.field_count; i++) {
            const PlumField *f = &s->as.tree.fields[i];
            visit_name(w, OCC_FIELD_DECL, f->name, f->span.offset, span_end(f->span, w->m->len),
                       NULL, s, f);
            walk_type(w, f->type);
            walk_expr(w, f->fallback);
        }
        for (size_t i = 0; i < s->as.tree.method_count; i++) {
            walk_branch(w, &s->as.tree.methods[i], s, NULL);
        }
        break;
    case PLUM_STMT_KIND:
        visit_name(w, OCC_SYMBOL, s->as.kind.name, start, end, s->symbol, NULL, NULL);
        for (size_t i = 0; i < s->as.kind.case_count; i++) {
            const PlumCase *c = &s->as.kind.cases[i];
            visit_name(w, OCC_CASE_DECL, c->name, c->span.offset, span_end(c->span, w->m->len),
                       NULL, s, c);
            for (size_t k = 0; k < c->param_count; k++) {
                const PlumParam *p = &c->params[k];
                visit_name(w, OCC_PARAM_DECL, p->name, p->span.offset, span_end(p->span, w->m->len),
                           NULL, s, p);
                walk_type(w, p->type);
            }
        }
        break;
    case PLUM_STMT_SEED:
        walk_seed(w, s);
        break;
    case PLUM_STMT_BREAK:
    case PLUM_STMT_CONTINUE:
    case PLUM_STMT_ERROR:
        break;
    }
    w->depth--;
}

/* What is at an offset: the shortest name around it, and the shortest
 * expression. */
typedef struct Finder {
    size_t offset;
    Occ name;
    bool has_name;
    Occ expr;
    bool has_expr;
} Finder;

static void find_visit(Walk *w, const Occ *o) {
    Finder *f = w->ctx;
    if (o->start > f->offset || f->offset > o->end || o->end == o->start) {
        return;
    }
    if (o->kind == OCC_EXPR) {
        if (!f->has_expr || o->end - o->start <= f->expr.end - f->expr.start) {
            f->expr = *o;
            f->has_expr = true;
        }
        return;
    }
    /* a name the cursor is in beats one it is just after */
    bool inside = f->offset < o->end;
    bool best_inside = f->has_name && f->offset < f->name.end;
    if (!f->has_name || (inside && !best_inside) ||
        (inside == best_inside && o->end - o->start < f->name.end - f->name.start)) {
        f->name = *o;
        f->has_name = true;
    }
}

static Finder find_at(const PlumModule *m, size_t offset) {
    Finder f;
    memset(&f, 0, sizeof f);
    f.offset = offset;
    Walk w = {m, find_visit, &f, 0};
    for (size_t i = 0; i < m->program.count; i++) {
        walk_stmt(&w, m->program.stmts[i]);
    }
    return f;
}

/* ------------------------------------------------------------------ */
/* Describing things                                                   */
/* ------------------------------------------------------------------ */

static const struct {
    const char *name;
    const char *signature;
    const char *doc;
} builtin_docs[] = {
    {"drop", "drop(values...)", "Print the values separated by spaces, then a newline."},
    {"ask", "ask(prompt: str = \"\") -> str?",
     "Print the prompt and read a line from standard input; `none` at the end of input."},
    {"len", "len(x) -> int", "The length of a string (in characters), list, map or set."},
    {"type_of", "type_of(x) -> str", "The name of a value's type."},
    {"assert", "assert(cond: bool, message: str = \"\")",
     "Throw an `AssertError` if `cond` is `bad`."},
    {"exit", "exit(code: int = 0)", "End the program."},
};

static void code_block(Buf *b, const char *code) {
    buf_str(b, "```plum\n");
    buf_str(b, code);
    buf_str(b, "\n```");
}

/* "x: int" for a name with a known type. */
static void name_and_type(Buf *b, const char *prefix, const char *name, const PlumTy *ty) {
    char tb[256];
    const char *t = type_text(ty, tb, sizeof tb);
    buf_str(b, prefix);
    buf_str(b, name);
    if (t != NULL) {
        buf_str(b, ": ");
        buf_str(b, t);
    }
}

static void native_function_doc(Buf *b, const char *name, const PlumNativeFunction *fn) {
    Buf code = {0};
    buf_str(&code, name);
    buf_str(&code, fn->signature);
    code_block(b, buf_text(&code));
    plum_array_free(&code);
    if (fn->doc != NULL && fn->doc[0] != '\0') {
        buf_str(b, "\n\n");
        buf_str(b, fn->doc);
    }
}

static void native_constant_doc(Buf *b, const PlumNativeConstant *k) {
    Buf code = {0};
    buf_str(&code, k->name);
    buf_str(&code, ": ");
    buf_str(&code, plum_native_constant_type(k));
    code_block(b, buf_text(&code));
    plum_array_free(&code);
    if (k->doc != NULL && k->doc[0] != '\0') {
        buf_str(b, "\n\n");
        buf_str(b, k->doc);
    }
}

/* "Kind.Case(radius: float)" */
static void case_text(const PlumModule *m, const PlumStmt *kind, const PlumCase *c, Buf *out) {
    buf_str(out, kind->as.kind.name);
    buf_str(out, ".");
    header_text(m, c->span.offset, span_end(c->span, m->len), out);
}

/*
 * The doc comment of what starts at `*at` in `m` (the module's own when
 * `at` is NULL), after a blank line. The file is lexed again for its
 * comments, which checking leaves out.
 */
static void doc_comment(Buf *b, const PlumModule *m, const PlumSpan *at) {
    if (m == NULL || m->text == NULL) {
        return;
    }
    PlumArena arena;
    PlumInterner names;
    PlumDiagList diags;
    PlumTokenList tokens = {0};
    PlumCommentList comments = {0};
    plum_arena_init(&arena);
    plum_interner_init(&names);
    plum_diag_init(&diags, &m->source);
    plum_lex_comments(&m->source, &arena, &names, &diags, &tokens, &comments);
    char *doc = at != NULL ? plum_doc_before(&m->source, &comments, *at)
                           : plum_doc_module(&m->source, &comments, &m->program);
    if (doc != NULL && doc[0] != '\0') {
        buf_str(b, "\n\n");
        buf_str(b, doc);
    }
    plum_free(doc);
    plum_array_free(&comments);
    plum_array_free(&tokens);
    plum_diag_free(&diags);
    plum_interner_free(&names);
    plum_arena_free(&arena);
}

static const PlumModule *seed_file(const PlumLspAnalysis *a, const PlumStmt *s);

/* Where a symbol comes from, when it is another file. */
static void from_note(Buf *b, const PlumLspAnalysis *a, const PlumModule *where) {
    if (where == NULL || where == a->main) {
        return;
    }
    const char *path = where->path;
    const char *slash = strrchr(path, '/');
    buf_str(b, "\n\nfrom `");
    buf_str(b, slash != NULL ? slash + 1 : path);
    buf_str(b, "`");
}

static void describe_symbol(Buf *b, const PlumLspAnalysis *a, const PlumSymbol *sym) {
    if (sym == NULL) {
        return;
    }
    const PlumModule *where = module_of(a, sym->decl, a->main);
    Buf code = {0};
    const char *note = NULL;
    const PlumModule *doc_in = NULL; /* where its doc comment is */
    PlumSpan doc_at = {0, 0, 0, 0};
    switch (sym->kind) {
    case PLUM_SYM_VAR:
        name_and_type(&code, "plant ", sym->name, sym->ty);
        break;
    case PLUM_SYM_CONST:
        name_and_type(&code, "root ", sym->name, sym->ty);
        if (sym->decl != NULL) {
            doc_in = where;
            doc_at = sym->decl->span;
        }
        break;
    case PLUM_SYM_PARAM:
        name_and_type(&code, "", sym->name, sym->ty);
        note = "parameter";
        break;
    case PLUM_SYM_LOOP:
        name_and_type(&code, "", sym->name, sym->ty);
        note = "loop variable";
        break;
    case PLUM_SYM_BINDING:
        name_and_type(&code, "", sym->name, sym->ty);
        break;
    case PLUM_SYM_FUNC:
    case PLUM_SYM_TREE:
    case PLUM_SYM_TRAIT:
    case PLUM_SYM_KIND:
        if (sym->decl != NULL) {
            header_text(where, sym->decl->span.offset, span_end(sym->decl->span, where->len),
                        &code);
            doc_in = where;
            doc_at = sym->decl->span;
        } else {
            buf_str(&code, "tree ");
            buf_str(&code, sym->name);
            if (strcmp(sym->name, "Error") != 0) {
                buf_str(&code, " : Error");
            }
            note = "A built-in error, with `message: str`.";
        }
        break;
    case PLUM_SYM_CASE: {
        const PlumCase *c = sym->owner != NULL ? find_case(sym->owner, sym->name) : NULL;
        if (c != NULL) {
            where = module_of(a, sym->owner->decl, a->main);
            case_text(where, sym->owner->decl, c, &code);
            doc_in = where;
            doc_at = c->span;
        } else {
            buf_str(&code, sym->name);
        }
        break;
    }
    case PLUM_SYM_BUILTIN_FUNC:
        for (size_t i = 0; i < sizeof builtin_docs / sizeof builtin_docs[0]; i++) {
            if (strcmp(builtin_docs[i].name, sym->name) == 0) {
                buf_str(&code, builtin_docs[i].signature);
                note = builtin_docs[i].doc;
            }
        }
        if (code.len == 0) {
            buf_str(&code, sym->name);
        }
        break;
    case PLUM_SYM_BUILTIN_TYPE:
        buf_str(&code, sym->name);
        note = "A built-in type.";
        break;
    case PLUM_SYM_MODULE: {
        buf_str(&code, "seed ");
        buf_str(&code, sym->decl != NULL ? sym->decl->as.seed.path : sym->name);
        const PlumNativeModule *nm = native_module_of(sym);
        note = nm != NULL ? nm->doc : NULL;
        where = NULL;
        if (sym->decl != NULL && sym->decl->kind == PLUM_STMT_SEED) {
            doc_in = seed_file(a, sym->decl); /* the module's own doc */
        }
        break;
    }
    case PLUM_SYM_NATIVE: {
        const PlumNativeModule *nm = native_module_of(sym);
        const PlumNativeFunction *fn = nm != NULL ? plum_native_function(nm, sym->name) : NULL;
        const PlumNativeConstant *k = nm != NULL ? plum_native_constant(nm, sym->name) : NULL;
        if (fn != NULL) {
            native_function_doc(b, sym->name, fn);
        } else if (k != NULL) {
            native_constant_doc(b, k);
        } else {
            code_block(b, sym->name);
        }
        plum_array_free(&code);
        return;
    }
    }
    code_block(b, buf_text(&code));
    plum_array_free(&code);
    if (doc_in != NULL) {
        doc_comment(b, doc_in, sym->kind == PLUM_SYM_MODULE ? NULL : &doc_at);
    }
    if (note != NULL && note[0] != '\0') {
        buf_str(b, "\n\n");
        buf_str(b, note);
    }
    from_note(b, a, where);
}

/* A member's header in a code block, and whose member it is. */
static void member_header(Buf *b, const PlumModule *where, PlumSpan span, const char *what,
                          const char *owner) {
    Buf code = {0};
    header_text(where, span.offset, span_end(span, where->len), &code);
    code_block(b, buf_text(&code));
    plum_array_free(&code);
    doc_comment(b, where, &span);
    buf_str(b, "\n\n");
    buf_str(b, what);
    buf_str(b, " of `");
    buf_str(b, owner);
    buf_str(b, "`");
}

/* a.b: what b is. */
static void describe_member(Buf *b, const PlumLspAnalysis *a, const PlumExpr *e) {
    const PlumExpr *obj = e->as.field.object;
    const char *name = e->as.field.name;
    char tb[256];
    if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL) {
        const PlumNativeModule *nm =
            obj->symbol->kind == PLUM_SYM_MODULE ? native_module_of(obj->symbol) : NULL;
        if (nm != NULL) {
            const PlumNativeFunction *fn = plum_native_function(nm, name);
            const PlumNativeConstant *k = plum_native_constant(nm, name);
            if (fn != NULL) {
                native_function_doc(b, name, fn);
            } else if (k != NULL) {
                native_constant_doc(b, k);
            }
            return;
        }
        if (obj->symbol->kind == PLUM_SYM_KIND) {
            const PlumCase *c = find_case(obj->symbol, name);
            if (c != NULL) {
                const PlumModule *where = module_of(a, obj->symbol->decl, a->main);
                Buf code = {0};
                case_text(where, obj->symbol->decl, c, &code);
                code_block(b, buf_text(&code));
                plum_array_free(&code);
                doc_comment(b, where, &c->span);
            }
            return;
        }
    }
    const PlumSymbol *tree = tree_of(obj != NULL ? obj->ty : NULL);
    if (tree != NULL) {
        const PlumSymbol *owner = NULL;
        const PlumField *f = find_field(tree, name, &owner);
        if (f != NULL) {
            member_header(b, module_of(a, owner->decl, a->main), f->span, "field", owner->name);
            return;
        }
        const PlumBranch *m = find_method(tree, name, &owner, 0);
        if (m != NULL) {
            member_header(b, module_of(a, owner->decl, a->main), m->span, "method", owner->name);
            return;
        }
    }
    bool method = false;
    const PlumNativeFunction *fn =
        obj != NULL && obj->ty != NULL ? plum_callee_native(e, &method) : NULL;
    if (fn != NULL) {
        native_function_doc(b, name, fn);
        return;
    }
    const char *text = type_text(e->ty, tb, sizeof tb);
    if (text != NULL) {
        Buf code = {0};
        buf_str(&code, name);
        buf_str(&code, ": ");
        buf_str(&code, text);
        code_block(b, buf_text(&code));
        plum_array_free(&code);
    }
}

static void describe(Buf *b, const PlumLspAnalysis *a, const Occ *o) {
    const PlumModule *m = a->main;
    switch (o->kind) {
    case OCC_SYMBOL:
        describe_symbol(b, a, o->symbol);
        break;
    case OCC_MEMBER:
        describe_member(b, a, o->expr);
        break;
    case OCC_FIELD_DECL: {
        const PlumField *f = o->item;
        member_header(b, m, f->span, "field", o->stmt->as.tree.name);
        break;
    }
    case OCC_METHOD_DECL: {
        const PlumBranch *br = o->item;
        member_header(b, m, br->span, "method", o->stmt->as.tree.name);
        break;
    }
    case OCC_CASE_DECL: {
        const PlumCase *c = o->item;
        Buf code = {0};
        case_text(m, o->stmt, c, &code);
        code_block(b, buf_text(&code));
        plum_array_free(&code);
        doc_comment(b, m, &c->span);
        break;
    }
    case OCC_SEED_PATH: {
        Buf code = {0};
        buf_str(&code, "seed ");
        buf_str(&code, o->stmt->as.seed.path);
        code_block(b, buf_text(&code));
        plum_array_free(&code);
        const PlumNativeModule *nm = plum_native_module(o->stmt->as.seed.path);
        if (o->stmt->as.seed.exports == NULL && nm != NULL && nm->doc != NULL) {
            buf_str(b, "\n\n");
            buf_str(b, nm->doc);
        }
        doc_comment(b, seed_file(a, o->stmt), NULL);
        break;
    }
    case OCC_SEED_NAME: {
        const PlumStmt *s = o->stmt;
        void *found = NULL;
        if (s->as.seed.exports != NULL) {
            if (plum_map_get(s->as.seed.exports, o->name, strlen(o->name), &found)) {
                describe_symbol(b, a, found);
            }
            break;
        }
        const PlumNativeModule *nm = plum_native_module(s->as.seed.path);
        const PlumNativeFunction *fn = nm != NULL ? plum_native_function(nm, o->name) : NULL;
        const PlumNativeConstant *k = nm != NULL ? plum_native_constant(nm, o->name) : NULL;
        if (fn != NULL) {
            native_function_doc(b, o->name, fn);
        } else if (k != NULL) {
            native_constant_doc(b, k);
        }
        break;
    }
    case OCC_PARAM_DECL:
    case OCC_ARG:
    case OCC_EXPR:
        break;
    }
}

void plum_lsp_hover(PlumJsonWriter *w, const PlumLspAnalysis *a, size_t offset) {
    Finder f = find_at(a->main, offset);
    Buf b = {0};
    size_t start = 0, end = 0;
    if (f.has_name) {
        describe(&b, a, &f.name);
        start = f.name.start;
        end = f.name.end;
    }
    if (b.len == 0 && f.has_expr) {
        char tb[256];
        const PlumTy *ty = f.expr.expr->ty;
        /* a call made for what it does says nothing worth showing */
        const char *t =
            ty != NULL && ty->kind != PLUM_TY_NONE ? type_text(ty, tb, sizeof tb) : NULL;
        if (t != NULL) {
            code_block(&b, t);
            start = f.expr.start;
            end = f.expr.end;
        }
    }
    if (b.len == 0) {
        plum_jw_null(w);
        plum_array_free(&b);
        return;
    }
    plum_jw_object(w);
    plum_jw_key(w, "contents");
    plum_jw_object(w);
    plum_jw_key(w, "kind");
    plum_jw_string(w, "markdown");
    plum_jw_key(w, "value");
    plum_jw_stringn(w, b.items, b.len);
    plum_jw_object_end(w);
    plum_jw_key(w, "range");
    write_module_range(w, a->main, start, end);
    plum_jw_object_end(w);
    plum_array_free(&b);
}

/* ------------------------------------------------------------------ */
/* Where names are declared                                            */
/* ------------------------------------------------------------------ */

/*
 * References and rename ask, for each name in a file, which declaration
 * it stands for: a file and the place of the declared name in it. Two
 * names stand for the same thing when those are the same, which holds
 * across files, each checked on its own.
 */
typedef struct Place {
    const PlumModule *m;
    size_t start, end;
} Place;

typedef enum PlaceKind {
    PLACE_OK,
    PLACE_NONE,    /* nothing there, or a name that isn't declared */
    PLACE_BUILTIN, /* drop, int, ValueError, ... */
    PLACE_STDLIB,  /* a standard module, or one of its functions */
    PLACE_FILE,    /* a file module's own name: the file */
} PlaceKind;

/* `name` as a word in a span of `m`. */
static PlaceKind name_place(const PlumModule *m, PlumSpan span, const char *name, Place *out) {
    long at = m != NULL && name != NULL
                  ? find_word(m->text, m->len, span.offset, span_end(span, m->len), name)
                  : -1;
    if (at < 0) {
        return PLACE_NONE;
    }
    out->m = m;
    out->start = (size_t)at;
    out->end = (size_t)at + strlen(name);
    return PLACE_OK;
}

/* The file a file module's seed loads, or NULL for a standard module. */
static const PlumModule *seed_file(const PlumLspAnalysis *a, const PlumStmt *s) {
    const PlumModule *file =
        s->as.seed.exports != NULL ? module_with_exports(a, s->as.seed.exports) : NULL;
    return file != NULL && file->path[0] != '<' ? file : NULL;
}

static PlaceKind symbol_place(const PlumLspAnalysis *a, const PlumSymbol *sym, Place *out) {
    if (sym == NULL || strcmp(sym->name, "self") == 0) {
        return PLACE_NONE;
    }
    if (sym->kind == PLUM_SYM_BUILTIN_FUNC || sym->kind == PLUM_SYM_BUILTIN_TYPE ||
        sym->span.line == 0) {
        return PLACE_BUILTIN;
    }
    if (sym->kind == PLUM_SYM_NATIVE) {
        return PLACE_STDLIB;
    }
    if (sym->kind == PLUM_SYM_MODULE) {
        const PlumStmt *s = sym->decl;
        if (s == NULL) {
            return PLACE_NONE;
        }
        if (s->as.seed.alias != NULL) {
            /* `seed ./shapes as sh`: the name is the alias, after the path */
            const PlumModule *m = module_of(a, s, a->main);
            size_t at = 0, after = s->span.offset;
            seed_path_at(m, s, &at, &after);
            PlumSpan rest = {(uint32_t)after, (uint32_t)(span_end(s->span, m->len) - after), 0, 0};
            return name_place(m, rest, s->as.seed.alias, out);
        }
        const PlumModule *file = seed_file(a, s);
        if (file == NULL) {
            return PLACE_STDLIB;
        }
        out->m = file;
        out->start = out->end = 0;
        return PLACE_FILE;
    }
    const PlumModule *m = module_of(a, sym->decl, a->main);
    if (sym->kind == PLUM_SYM_CASE && sym->owner != NULL) {
        m = module_of(a, sym->owner->decl, a->main);
    }
    return name_place(m, sym->span, sym->name, out);
}

/* a.b: the declaration of b. */
static PlaceKind member_place(const PlumLspAnalysis *a, const PlumExpr *e, Place *out) {
    const PlumExpr *obj = e->as.field.object;
    const char *name = e->as.field.name;
    if (name == NULL) {
        return PLACE_NONE;
    }
    if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL) {
        if (obj->symbol->kind == PLUM_SYM_KIND) {
            const PlumCase *c = find_case(obj->symbol, name);
            return c != NULL
                       ? name_place(module_of(a, obj->symbol->decl, a->main), c->span, name, out)
                       : PLACE_NONE;
        }
        if (obj->symbol->kind == PLUM_SYM_MODULE && native_module_of(obj->symbol) != NULL) {
            return PLACE_STDLIB;
        }
    }
    const PlumSymbol *tree = tree_of(obj != NULL ? obj->ty : NULL);
    const PlumSymbol *owner = NULL;
    const PlumField *f = tree != NULL ? find_field(tree, name, &owner) : NULL;
    if (f != NULL) {
        return name_place(module_of(a, owner->decl, a->main), f->span, name, out);
    }
    const PlumBranch *m = tree != NULL ? find_method(tree, name, &owner, 0) : NULL;
    if (m != NULL) {
        return name_place(module_of(a, owner->decl, a->main), m->span, name, out);
    }
    if (tree != NULL && tree->decl == NULL) {
        return PLACE_BUILTIN; /* a built-in error's message */
    }
    bool method = false;
    if (obj != NULL && obj->ty != NULL && plum_callee_native(e, &method) != NULL) {
        return PLACE_STDLIB;
    }
    return PLACE_NONE;
}

/* `name` among parameters declared in module `m`. */
static PlaceKind param_place(const PlumModule *m, const PlumParam *params, size_t count,
                             const char *name, Place *out) {
    for (size_t i = 0; i < count; i++) {
        if (params[i].name != NULL && strcmp(params[i].name, name) == 0) {
            return name_place(m, params[i].span, name, out);
        }
    }
    return PLACE_NONE;
}

/* A tree's constructor parameter: init's, or a field. */
static PlaceKind constructor_place(const PlumLspAnalysis *a, const PlumSymbol *tree,
                                   const char *name, Place *out) {
    const PlumSymbol *owner = NULL;
    const PlumBranch *init = find_method(tree, "init", &owner, 0);
    if (init != NULL) {
        return param_place(module_of(a, owner->decl, a->main), init->params, init->param_count,
                           name, out);
    }
    const PlumField *f = find_field(tree, name, &owner);
    if (f != NULL) {
        return name_place(module_of(a, owner->decl, a->main), f->span, name, out);
    }
    return tree->decl == NULL ? PLACE_BUILTIN : PLACE_NONE;
}

/* f(size: 2): the parameter (or field) `size` is. */
static PlaceKind arg_place(const PlumLspAnalysis *a, const PlumExpr *call, const char *name,
                           Place *out) {
    const PlumExpr *callee = call->as.call.callee;
    if (callee == NULL) {
        return PLACE_NONE;
    }
    const PlumSymbol *kind = NULL;
    const char *case_name = NULL;
    if (callee->kind == PLUM_EXPR_NAME && callee->symbol != NULL) {
        const PlumSymbol *sym = callee->symbol;
        switch (sym->kind) {
        case PLUM_SYM_FUNC:
            if (sym->decl != NULL && sym->decl->kind == PLUM_STMT_BRANCH) {
                const PlumBranch *b = &sym->decl->as.branch;
                return param_place(module_of(a, sym->decl, a->main), b->params, b->param_count,
                                   name, out);
            }
            return PLACE_NONE;
        case PLUM_SYM_TREE:
            return constructor_place(a, sym, name, out);
        case PLUM_SYM_CASE:
            kind = sym->owner;
            case_name = sym->name;
            break;
        case PLUM_SYM_BUILTIN_FUNC:
        case PLUM_SYM_BUILTIN_TYPE:
            return PLACE_BUILTIN;
        case PLUM_SYM_NATIVE:
            return PLACE_STDLIB;
        default:
            return PLACE_NONE;
        }
    } else if (callee->kind == PLUM_EXPR_FIELD && callee->as.field.name != NULL) {
        const PlumExpr *obj = callee->as.field.object;
        if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL &&
            obj->symbol->kind == PLUM_SYM_KIND) {
            kind = obj->symbol;
            case_name = callee->as.field.name;
        } else {
            const PlumSymbol *tree = tree_of(obj != NULL ? obj->ty : NULL);
            const PlumSymbol *owner = NULL;
            const PlumBranch *m =
                tree != NULL ? find_method(tree, callee->as.field.name, &owner, 0) : NULL;
            if (m != NULL) {
                return param_place(module_of(a, owner->decl, a->main), m->params, m->param_count,
                                   name, out);
            }
            Place ignored;
            return member_place(a, callee, &ignored) == PLACE_STDLIB ? PLACE_STDLIB : PLACE_NONE;
        }
    }
    const PlumCase *c = kind != NULL ? find_case(kind, case_name) : NULL;
    if (c == NULL) {
        return PLACE_NONE;
    }
    return param_place(module_of(a, kind->decl, a->main), c->params, c->param_count, name, out);
}

/* What an occurrence in module `m` (of analysis `a`) stands for. */
static PlaceKind occ_place(const PlumLspAnalysis *a, const PlumModule *m, const Occ *o,
                           Place *out) {
    switch (o->kind) {
    case OCC_SYMBOL:
        return symbol_place(a, o->symbol, out);
    case OCC_MEMBER:
        return member_place(a, o->expr, out);
    case OCC_FIELD_DECL:
    case OCC_METHOD_DECL:
    case OCC_CASE_DECL:
    case OCC_PARAM_DECL:
        out->m = m;
        out->start = o->start;
        out->end = o->end;
        return PLACE_OK;
    case OCC_SEED_PATH: {
        const PlumModule *file = seed_file(a, o->stmt);
        if (file == NULL) {
            return PLACE_STDLIB;
        }
        out->m = file;
        out->start = out->end = 0;
        return PLACE_FILE;
    }
    case OCC_SEED_NAME: {
        void *found = NULL;
        const PlumMap *exports = o->stmt->as.seed.exports;
        if (exports != NULL && plum_map_get(exports, o->name, strlen(o->name), &found)) {
            return symbol_place(a, found, out);
        }
        return exports == NULL ? PLACE_STDLIB : PLACE_NONE;
    }
    case OCC_ARG:
        return arg_place(a, o->expr, o->name, out);
    case OCC_EXPR:
        break;
    }
    return PLACE_NONE;
}

/* ------------------------------------------------------------------ */
/* Definition                                                          */
/* ------------------------------------------------------------------ */

static void write_location(PlumJsonWriter *w, const PlumModule *m, size_t start, size_t end,
                           const PlumLspNamer *namer) {
    char *uri = m != NULL ? namer->uri(namer->ctx, m->path) : NULL;
    if (uri == NULL) {
        plum_jw_null(w);
        return;
    }
    plum_jw_object(w);
    plum_jw_key(w, "uri");
    plum_jw_string(w, uri);
    plum_jw_key(w, "range");
    write_module_range(w, m, start, end);
    plum_jw_object_end(w);
    plum_free(uri);
}

/* The location of `name` within a span of module `m`. */
static void write_name_location(PlumJsonWriter *w, const PlumModule *m, PlumSpan span,
                                const char *name, const PlumLspNamer *namer) {
    long at = find_word(m->text, m->len, span.offset, span_end(span, m->len), name);
    size_t start = at >= 0 ? (size_t)at : span.offset;
    size_t end = at >= 0 ? start + strlen(name) : start;
    write_location(w, m, start, end, namer);
}

static void symbol_location(PlumJsonWriter *w, const PlumLspAnalysis *a, const PlumSymbol *sym,
                            const PlumLspNamer *namer) {
    if (sym == NULL || sym->span.line == 0 || strcmp(sym->name, "self") == 0) {
        plum_jw_null(w); /* built-ins are declared nowhere */
        return;
    }
    if (sym->kind == PLUM_SYM_MODULE && sym->decl != NULL && sym->decl->as.seed.exports != NULL) {
        /* the file a module name stands for */
        const PlumModule *m = module_with_exports(a, sym->decl->as.seed.exports);
        if (m != NULL && m->path[0] != '<') {
            write_location(w, m, 0, 0, namer);
        } else {
            plum_jw_null(w);
        }
        return;
    }
    const PlumModule *m = module_of(a, sym->decl, a->main);
    if (sym->kind == PLUM_SYM_CASE && sym->owner != NULL) {
        m = module_of(a, sym->owner->decl, a->main);
    }
    write_name_location(w, m, sym->span, sym->name, namer);
}

void plum_lsp_definition(PlumJsonWriter *w, const PlumLspAnalysis *a, size_t offset,
                         const PlumLspNamer *namer) {
    Finder f = find_at(a->main, offset);
    if (!f.has_name) {
        plum_jw_null(w);
        return;
    }
    const Occ *o = &f.name;
    switch (o->kind) {
    case OCC_SYMBOL:
        symbol_location(w, a, o->symbol, namer);
        return;
    case OCC_MEMBER: {
        const PlumExpr *obj = o->expr->as.field.object;
        const char *name = o->expr->as.field.name;
        if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL &&
            obj->symbol->kind == PLUM_SYM_KIND) {
            const PlumCase *c = find_case(obj->symbol, name);
            if (c != NULL) {
                write_name_location(w, module_of(a, obj->symbol->decl, a->main), c->span, name,
                                    namer);
                return;
            }
        }
        const PlumSymbol *tree = tree_of(obj != NULL ? obj->ty : NULL);
        const PlumSymbol *owner = NULL;
        const PlumField *fd = tree != NULL ? find_field(tree, name, &owner) : NULL;
        if (fd != NULL) {
            write_name_location(w, module_of(a, owner->decl, a->main), fd->span, name, namer);
            return;
        }
        const PlumBranch *m = tree != NULL ? find_method(tree, name, &owner, 0) : NULL;
        if (m != NULL) {
            write_name_location(w, module_of(a, owner->decl, a->main), m->span, name, namer);
            return;
        }
        break;
    }
    case OCC_FIELD_DECL:
    case OCC_METHOD_DECL:
    case OCC_CASE_DECL:
    case OCC_PARAM_DECL:
        write_location(w, a->main, o->start, o->end, namer);
        return;
    case OCC_ARG: {
        Place p;
        if (arg_place(a, o->expr, o->name, &p) == PLACE_OK) {
            write_location(w, p.m, p.start, p.end, namer);
            return;
        }
        break;
    }
    case OCC_SEED_PATH:
        if (o->stmt->as.seed.exports != NULL) {
            const PlumModule *m = module_with_exports(a, o->stmt->as.seed.exports);
            if (m != NULL && m->path[0] != '<') {
                write_location(w, m, 0, 0, namer);
                return;
            }
        }
        break;
    case OCC_SEED_NAME: {
        void *found = NULL;
        const PlumMap *exports = o->stmt->as.seed.exports;
        if (exports != NULL && plum_map_get(exports, o->name, strlen(o->name), &found)) {
            symbol_location(w, a, found, namer);
            return;
        }
        break;
    }
    case OCC_EXPR:
        break;
    }
    plum_jw_null(w);
}

/* ------------------------------------------------------------------ */
/* References and rename                                               */
/* ------------------------------------------------------------------ */

static bool is_keyword(const char *word, size_t len) {
    const char *k;
    for (size_t i = 0; (k = plum_keyword_at(i)) != NULL; i++) {
        if (strlen(k) == len && memcmp(k, word, len) == 0) {
            return true;
        }
    }
    return false;
}

bool plum_lsp_target(const PlumLspAnalysis *a, size_t offset, PlumLspTarget *t) {
    memset(t, 0, sizeof *t);
    const PlumModule *main = a->main;
    Finder f = find_at(main, offset);
    if (!f.has_name) {
        size_t from = offset < main->len ? offset : main->len, to = from;
        while (from > 0 && is_name_char(main->text[from - 1])) {
            from--;
        }
        while (to < main->len && is_name_char(main->text[to])) {
            to++;
        }
        if (to > from && is_keyword(main->text + from, to - from)) {
            snprintf(t->no_rename, sizeof t->no_rename,
                     "'%.*s' is a keyword; only names can be renamed", (int)(to - from),
                     main->text + from);
        } else {
            snprintf(t->no_rename, sizeof t->no_rename, "there is no name here to rename");
        }
        return false;
    }
    const Occ *o = &f.name;
    int n = (int)(o->end - o->start);
    const char *word = main->text + o->start;
    Place p;
    switch (occ_place(a, main, o, &p)) {
    case PLACE_OK:
        break;
    case PLACE_FILE: {
        const char *slash = strrchr(p.m->path, '/');
        snprintf(t->no_rename, sizeof t->no_rename,
                 "'%.*s' is the file %s; rename the file, or name the module with `seed ... as`", n,
                 word, slash != NULL ? slash + 1 : p.m->path);
        break;
    }
    case PLACE_BUILTIN:
        snprintf(t->no_rename, sizeof t->no_rename,
                 "'%.*s' is built into Plumlang; it can't be renamed", n, word);
        return false;
    case PLACE_STDLIB:
        snprintf(t->no_rename, sizeof t->no_rename,
                 "'%.*s' belongs to the standard library; it can't be renamed", n, word);
        return false;
    case PLACE_NONE:
        snprintf(t->no_rename, sizeof t->no_rename,
                 "'%.*s' isn't declared anywhere, so there is nothing to rename", n, word);
        return false;
    }
    t->path = plum_strdup(p.m->path);
    t->start = p.start;
    t->end = p.end;
    t->name = p.end > p.start ? plum_strndup(p.m->text + p.start, p.end - p.start) : NULL;
    t->at_start = o->start;
    t->at_end = o->end;
    return true;
}

void plum_lsp_target_free(PlumLspTarget *t) {
    plum_free(t->path);
    plum_free(t->name);
    t->path = NULL;
    t->name = NULL;
}

typedef struct Refs {
    const PlumLspAnalysis *a;
    const PlumLspTarget *t;
    bool declaration;
    PlumLspRanges *out;
} Refs;

static void refs_visit(Walk *w, const Occ *o) {
    Refs *r = w->ctx;
    const PlumLspTarget *t = r->t;
    if (o->kind == OCC_EXPR) {
        return;
    }
    /* only a name spelled as the declared one can stand for it */
    size_t n = t->name != NULL ? strlen(t->name) : 0;
    if (t->name != NULL &&
        (o->end - o->start != n || memcmp(w->m->text + o->start, t->name, n) != 0)) {
        return;
    }
    Place p;
    PlaceKind k = occ_place(r->a, w->m, o, &p);
    if ((k != PLACE_OK && k != PLACE_FILE) || p.start != t->start ||
        !plum_lsp_same_path(p.m->path, t->path)) {
        return;
    }
    if (!r->declaration && p.m == w->m && p.start == o->start) {
        return;
    }
    for (size_t i = 0; i < r->out->len; i++) {
        if (r->out->items[i].start == o->start) {
            return;
        }
    }
    PlumLspRange range = {o->start, o->end};
    plum_array_push(r->out, range);
}

static int by_start(const void *x, const void *y) {
    size_t a = ((const PlumLspRange *)x)->start, b = ((const PlumLspRange *)y)->start;
    return a < b ? -1 : a > b;
}

void plum_lsp_references(const PlumLspAnalysis *a, const PlumLspTarget *t, bool declaration,
                         PlumLspRanges *out) {
    Refs r = {a, t, declaration, out};
    Walk w = {a->main, refs_visit, &r, 0};
    for (size_t i = 0; i < a->main->program.count; i++) {
        walk_stmt(&w, a->main->program.stmts[i]);
    }
    if (out->len > 1) {
        qsort(out->items, out->len, sizeof *out->items, by_start);
    }
}

bool plum_lsp_valid_name(const char *name, size_t len, char *why, size_t size) {
    bool ok = len > 0 && !(name[0] >= '0' && name[0] <= '9');
    for (size_t i = 0; ok && i < len; i++) {
        ok = is_name_char(name[i]);
    }
    if (!ok) {
        snprintf(why, size,
                 "'%.*s' isn't a name: use letters, digits and '_', not starting with a digit",
                 (int)(len < 64 ? len : 64), name);
        return false;
    }
    if (is_keyword(name, len) || (len == 1 && name[0] == '_')) {
        snprintf(why, size, "'%.*s' is a keyword; choose another name", (int)len, name);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Document symbols                                                    */
/* ------------------------------------------------------------------ */

enum {
    SK_MODULE = 2,
    SK_CLASS = 5,
    SK_METHOD = 6,
    SK_FIELD = 8,
    SK_ENUM = 10,
    SK_INTERFACE = 11,
    SK_FUNCTION = 12,
    SK_VARIABLE = 13,
    SK_CONSTANT = 14,
    SK_ENUM_MEMBER = 22,
};

static void write_symbol_start(PlumJsonWriter *w, const PlumLspLines *lines, const PlumModule *m,
                               const char *name, int kind, PlumSpan span, const char *detail) {
    size_t start = span.offset < m->len ? span.offset : m->len;
    size_t end = span_end(span, m->len);
    long at = find_word(m->text, m->len, start, end, name);
    size_t sel = at >= 0 ? (size_t)at : start;
    size_t sel_end = at >= 0 ? sel + strlen(name) : start;
    plum_jw_object(w);
    plum_jw_key(w, "name");
    plum_jw_string(w, name);
    if (detail != NULL && detail[0] != '\0') {
        plum_jw_key(w, "detail");
        plum_jw_string(w, detail);
    }
    plum_jw_key(w, "kind");
    plum_jw_int(w, kind);
    plum_jw_key(w, "range");
    write_range(w, lines, start, end);
    plum_jw_key(w, "selectionRange");
    write_range(w, lines, sel, sel_end);
}

static void write_leaf_symbol(PlumJsonWriter *w, const PlumLspLines *lines, const PlumModule *m,
                              const char *name, int kind, PlumSpan span, const char *detail) {
    if (name == NULL || name[0] == '\0') {
        return;
    }
    write_symbol_start(w, lines, m, name, kind, span, detail);
    plum_jw_object_end(w);
}

void plum_lsp_document_symbols(PlumJsonWriter *w, const PlumLspAnalysis *a) {
    const PlumModule *m = a->main;
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, m->text, m->len);
    plum_jw_array(w);
    for (size_t i = 0; i < m->program.count; i++) {
        const PlumStmt *s = m->program.stmts[i];
        Buf detail = {0};
        switch (s->kind) {
        case PLUM_STMT_DECL: {
            char tb[256];
            const char *t = s->symbol != NULL ? type_text(s->symbol->ty, tb, sizeof tb) : NULL;
            write_leaf_symbol(w, &lines, m, s->as.decl.name,
                              s->as.decl.constant ? SK_CONSTANT : SK_VARIABLE, s->span, t);
            break;
        }
        case PLUM_STMT_BRANCH:
            header_text(m, s->span.offset, span_end(s->span, m->len), &detail);
            write_leaf_symbol(w, &lines, m, s->as.branch.name, SK_FUNCTION, s->span,
                              buf_text(&detail));
            break;
        case PLUM_STMT_SEED:
            write_leaf_symbol(w, &lines, m, s->as.seed.path, SK_MODULE, s->span, NULL);
            break;
        case PLUM_STMT_TREE:
        case PLUM_STMT_TRAIT:
            if (s->as.tree.name == NULL) {
                break;
            }
            write_symbol_start(w, &lines, m, s->as.tree.name,
                               s->kind == PLUM_STMT_TREE ? SK_CLASS : SK_INTERFACE, s->span, NULL);
            plum_jw_key(w, "children");
            plum_jw_array(w);
            for (size_t k = 0; k < s->as.tree.field_count; k++) {
                const PlumField *f = &s->as.tree.fields[k];
                Buf fd = {0};
                header_text(m, f->span.offset, span_end(f->span, m->len), &fd);
                write_leaf_symbol(w, &lines, m, f->name, SK_FIELD, f->span, buf_text(&fd));
                plum_array_free(&fd);
            }
            for (size_t k = 0; k < s->as.tree.method_count; k++) {
                const PlumBranch *b = &s->as.tree.methods[k];
                Buf md = {0};
                header_text(m, b->span.offset, span_end(b->span, m->len), &md);
                write_leaf_symbol(w, &lines, m, b->name, SK_METHOD, b->span, buf_text(&md));
                plum_array_free(&md);
            }
            plum_jw_array_end(w);
            plum_jw_object_end(w);
            break;
        case PLUM_STMT_KIND:
            if (s->as.kind.name == NULL) {
                break;
            }
            write_symbol_start(w, &lines, m, s->as.kind.name, SK_ENUM, s->span, NULL);
            plum_jw_key(w, "children");
            plum_jw_array(w);
            for (size_t k = 0; k < s->as.kind.case_count; k++) {
                const PlumCase *c = &s->as.kind.cases[k];
                write_leaf_symbol(w, &lines, m, c->name, SK_ENUM_MEMBER, c->span, NULL);
            }
            plum_jw_array_end(w);
            plum_jw_object_end(w);
            break;
        default:
            break;
        }
        plum_array_free(&detail);
    }
    plum_jw_array_end(w);
    plum_lsp_lines_free(&lines);
}

/* ------------------------------------------------------------------ */
/* Completion                                                          */
/* ------------------------------------------------------------------ */

enum {
    CK_METHOD = 2,
    CK_FUNCTION = 3,
    CK_FIELD = 5,
    CK_VARIABLE = 6,
    CK_CLASS = 7,
    CK_INTERFACE = 8,
    CK_MODULE = 9,
    CK_ENUM = 13,
    CK_KEYWORD = 14,
    CK_ENUM_MEMBER = 20,
    CK_CONSTANT = 21,
    CK_STRUCT = 22,
};

typedef struct Item {
    char *label; /* owned */
    int kind;
    char *detail;    /* owned, or NULL */
    const char *doc; /* static, or NULL */
} Item;

typedef struct Items {
    PLUM_ARRAY(Item) list;
    PlumMap seen; /* label -> index + 1; keys are the items' own labels */
} Items;

/* Add an item; a later one with the same label replaces the earlier (an
 * inner name hides an outer one). */
static void add_item(Items *items, const char *label, int kind, const char *detail,
                     const char *doc) {
    if (label == NULL || label[0] == '\0') {
        return;
    }
    char *d = detail != NULL && detail[0] != '\0' ? plum_strdup(detail) : NULL;
    void *found = NULL;
    if (plum_map_get(&items->seen, label, strlen(label), &found)) {
        Item *it = &items->list.items[(size_t)(uintptr_t)found - 1];
        plum_free(it->detail);
        it->kind = kind;
        it->detail = d;
        it->doc = doc;
        return;
    }
    Item it = {plum_strdup(label), kind, d, doc};
    plum_array_push(&items->list, it);
    /* the key is the item's label, which never moves */
    plum_map_set(&items->seen, it.label, strlen(it.label), (void *)(uintptr_t)items->list.len);
}

/* Add only if nothing has that label yet. */
static void add_item_once(Items *items, const char *label, int kind, const char *detail,
                          const char *doc) {
    if (label != NULL && !plum_map_get(&items->seen, label, strlen(label), NULL)) {
        add_item(items, label, kind, detail, doc);
    }
}

static int symbol_item_kind(const PlumSymbol *sym) {
    switch (sym->kind) {
    case PLUM_SYM_CONST:
        return CK_CONSTANT;
    case PLUM_SYM_FUNC:
    case PLUM_SYM_BUILTIN_FUNC:
    case PLUM_SYM_NATIVE:
        return CK_FUNCTION;
    case PLUM_SYM_TREE:
        return CK_CLASS;
    case PLUM_SYM_TRAIT:
        return CK_INTERFACE;
    case PLUM_SYM_KIND:
        return CK_ENUM;
    case PLUM_SYM_CASE:
        return CK_ENUM_MEMBER;
    case PLUM_SYM_BUILTIN_TYPE:
        return CK_STRUCT;
    case PLUM_SYM_MODULE:
        return CK_MODULE;
    default:
        return CK_VARIABLE;
    }
}

static void add_symbol(Items *items, const PlumLspAnalysis *a, const PlumSymbol *sym) {
    if (sym == NULL) {
        return;
    }
    Buf detail = {0};
    char tb[256];
    if ((sym->kind == PLUM_SYM_FUNC || sym->kind == PLUM_SYM_TREE || sym->kind == PLUM_SYM_TRAIT ||
         sym->kind == PLUM_SYM_KIND) &&
        sym->decl != NULL) {
        const PlumModule *m = module_of(a, sym->decl, a->main);
        header_text(m, sym->decl->span.offset, span_end(sym->decl->span, m->len), &detail);
    } else if (sym->kind == PLUM_SYM_CASE && sym->owner != NULL &&
               find_case(sym->owner, sym->name) != NULL) {
        case_text(module_of(a, sym->owner->decl, a->main), sym->owner->decl,
                  find_case(sym->owner, sym->name), &detail);
    } else if (type_text(sym->ty, tb, sizeof tb) != NULL) {
        buf_str(&detail, tb);
    }
    add_item(items, sym->name, symbol_item_kind(sym), detail.items, NULL);
    plum_array_free(&detail);
}

/* What a seed brings into scope. */
static void add_seed(Items *items, const PlumLspAnalysis *a, const PlumStmt *s) {
    if (s->as.seed.name_count == 0) {
        add_symbol(items, a, s->symbol);
        return;
    }
    const PlumNativeModule *nm =
        s->as.seed.exports == NULL ? plum_native_module(s->as.seed.path) : NULL;
    for (size_t i = 0; i < s->as.seed.name_count; i++) {
        const char *name = s->as.seed.names[i];
        void *found = NULL;
        const PlumNativeFunction *fn = nm != NULL ? plum_native_function(nm, name) : NULL;
        const PlumNativeConstant *k = nm != NULL ? plum_native_constant(nm, name) : NULL;
        if (s->as.seed.exports != NULL &&
            plum_map_get(s->as.seed.exports, name, strlen(name), &found)) {
            add_symbol(items, a, found);
        } else if (fn != NULL) {
            add_item(items, name, CK_FUNCTION, fn->signature, fn->doc);
        } else if (k != NULL) {
            add_item(items, name, CK_CONSTANT, plum_native_constant_type(k), k->doc);
        }
    }
}

static bool contains(PlumSpan span, size_t offset) {
    return span.offset <= offset && offset <= (size_t)span.offset + span.length;
}

static void scope_stmt(Items *items, const PlumLspAnalysis *a, const PlumStmt *s, size_t offset,
                       int depth);

static void add_binding(Items *items, const PlumLspAnalysis *a, const PlumExpr *p) {
    if (p != NULL && p->kind == PLUM_EXPR_NAME && p->symbol != NULL &&
        p->symbol->kind == PLUM_SYM_BINDING) {
        add_symbol(items, a, p->symbol);
    }
}

/* The names a lambda or match arm around the offset adds. */
static void scope_expr(Items *items, const PlumLspAnalysis *a, const PlumExpr *e, size_t offset,
                       int depth) {
    if (e == NULL || depth > 200 || !contains(e->span, offset)) {
        return;
    }
    switch (e->kind) {
    case PLUM_EXPR_LAMBDA:
        for (size_t i = 0; i < e->as.lambda.param_count; i++) {
            add_symbol(items, a, e->as.lambda.params[i].symbol);
        }
        scope_expr(items, a, e->as.lambda.body, offset, depth + 1);
        scope_stmt(items, a, e->as.lambda.block, offset, depth + 1);
        break;
    case PLUM_EXPR_MATCH:
        scope_expr(items, a, e->as.match.subject, offset, depth + 1);
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            const PlumMatchArm *arm = &e->as.match.arms[i];
            if (arm->body == NULL || !contains(arm->body->span, offset)) {
                continue;
            }
            for (size_t k = 0; k < arm->pattern_count; k++) {
                const PlumExpr *p = arm->patterns[k];
                if (p != NULL && p->kind == PLUM_EXPR_CALL) {
                    for (size_t j = 0; j < p->as.call.arg_count; j++) {
                        add_binding(items, a, p->as.call.args[j].value);
                    }
                } else {
                    add_binding(items, a, p);
                }
            }
            scope_stmt(items, a, arm->body, offset, depth + 1);
        }
        break;
    case PLUM_EXPR_CALL:
        scope_expr(items, a, e->as.call.callee, offset, depth + 1);
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            scope_expr(items, a, e->as.call.args[i].value, offset, depth + 1);
        }
        break;
    case PLUM_EXPR_LIST:
        for (size_t i = 0; i < e->as.list.count; i++) {
            scope_expr(items, a, e->as.list.items[i], offset, depth + 1);
        }
        break;
    case PLUM_EXPR_BINARY:
        scope_expr(items, a, e->as.binary.left, offset, depth + 1);
        scope_expr(items, a, e->as.binary.right, offset, depth + 1);
        break;
    case PLUM_EXPR_FIELD:
        scope_expr(items, a, e->as.field.object, offset, depth + 1);
        break;
    case PLUM_EXPR_SPAWN:
    case PLUM_EXPR_TRY:
        scope_expr(items, a, e->as.inner, offset, depth + 1);
        break;
    default:
        break;
    }
}

static void scope_function(Items *items, const PlumLspAnalysis *a, const PlumBranch *b,
                           size_t offset, int depth) {
    if (!contains(b->span, offset)) {
        return;
    }
    for (size_t i = 0; i < b->param_count; i++) {
        add_symbol(items, a, b->params[i].symbol);
    }
    scope_stmt(items, a, b->body, offset, depth + 1);
    scope_expr(items, a, b->arrow, offset, depth + 1);
}

/* The local names visible at `offset` inside `s`. */
static void scope_stmt(Items *items, const PlumLspAnalysis *a, const PlumStmt *s, size_t offset,
                       int depth) {
    if (s == NULL || depth > 200) {
        return;
    }
    switch (s->kind) {
    case PLUM_STMT_BLOCK:
        for (size_t i = 0; i < s->as.block.count; i++) {
            const PlumStmt *inner = s->as.block.stmts[i];
            if (inner == NULL || inner->span.offset > offset) {
                break;
            }
            if (inner->kind == PLUM_STMT_DECL && !contains(inner->span, offset)) {
                add_symbol(items, a, inner->symbol);
            }
            if (contains(inner->span, offset)) {
                scope_stmt(items, a, inner, offset, depth + 1);
            }
        }
        break;
    case PLUM_STMT_EXPR:
    case PLUM_STMT_THROW:
    case PLUM_STMT_RETURN:
        scope_expr(items, a, s->as.expr, offset, depth + 1);
        break;
    case PLUM_STMT_DECL:
        scope_expr(items, a, s->as.decl.value, offset, depth + 1);
        break;
    case PLUM_STMT_ASSIGN:
        scope_expr(items, a, s->as.assign.value, offset, depth + 1);
        break;
    case PLUM_STMT_IF:
        scope_stmt(items, a, s->as.if_.then, offset, depth + 1);
        scope_stmt(items, a, s->as.if_.otherwise, offset, depth + 1);
        break;
    case PLUM_STMT_IF_PLANT:
        if (s->as.if_.then != NULL && contains(s->as.if_.then->span, offset)) {
            add_symbol(items, a, s->symbol);
            scope_stmt(items, a, s->as.if_.then, offset, depth + 1);
        }
        scope_stmt(items, a, s->as.if_.otherwise, offset, depth + 1);
        break;
    case PLUM_STMT_WHILE:
        scope_stmt(items, a, s->as.while_.body, offset, depth + 1);
        break;
    case PLUM_STMT_FOR:
        if (s->as.for_.body != NULL && contains(s->as.for_.body->span, offset)) {
            add_symbol(items, a, s->symbol);
            add_symbol(items, a, s->symbol2);
            scope_stmt(items, a, s->as.for_.body, offset, depth + 1);
        }
        break;
    case PLUM_STMT_TRY:
        scope_stmt(items, a, s->as.try_.body, offset, depth + 1);
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            const PlumCatch *c = &s->as.try_.catches[i];
            if (c->body != NULL && contains(c->body->span, offset)) {
                add_symbol(items, a, c->symbol);
                scope_stmt(items, a, c->body, offset, depth + 1);
            }
        }
        scope_stmt(items, a, s->as.try_.finally, offset, depth + 1);
        break;
    case PLUM_STMT_BRANCH:
        scope_function(items, a, &s->as.branch, offset, depth);
        break;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
        for (size_t i = 0; i < s->as.tree.method_count; i++) {
            scope_function(items, a, &s->as.tree.methods[i], offset, depth);
        }
        break;
    default:
        break;
    }
}

static void add_scope(Items *items, const PlumLspAnalysis *a, size_t offset) {
    const PlumProgram *p = &a->main->program;
    /* every top-level name (function bodies see them all), then the
     * locals around the offset, which hide them */
    for (size_t i = 0; i < p->count; i++) {
        const PlumStmt *s = p->stmts[i];
        if (s->kind == PLUM_STMT_SEED) {
            add_seed(items, a, s);
        } else if (s->kind == PLUM_STMT_DECL || s->kind == PLUM_STMT_BRANCH ||
                   s->kind == PLUM_STMT_TREE || s->kind == PLUM_STMT_TRAIT ||
                   s->kind == PLUM_STMT_KIND) {
            add_symbol(items, a, s->symbol);
        }
    }
    for (size_t i = 0; i < p->count; i++) {
        if (contains(p->stmts[i]->span, offset)) {
            scope_stmt(items, a, p->stmts[i], offset, 0);
        }
    }
    PlumSymbolKind kind;
    const char *name;
    for (size_t i = 0; (name = plum_builtin_at(i, &kind)) != NULL; i++) {
        const char *detail = NULL;
        const char *doc = NULL;
        for (size_t k = 0; k < sizeof builtin_docs / sizeof builtin_docs[0]; k++) {
            if (strcmp(builtin_docs[k].name, name) == 0) {
                detail = builtin_docs[k].signature;
                doc = builtin_docs[k].doc;
            }
        }
        add_item_once(items, name,
                      kind == PLUM_SYM_BUILTIN_FUNC   ? CK_FUNCTION
                      : kind == PLUM_SYM_BUILTIN_TYPE ? CK_STRUCT
                                                      : CK_CLASS,
                      detail, doc);
    }
    for (size_t i = 0; (name = plum_keyword_at(i)) != NULL; i++) {
        add_item_once(items, name, CK_KEYWORD, NULL, NULL);
    }
}

static void add_native_members(Items *items, const PlumNativeModule *m) {
    if (m == NULL) {
        return;
    }
    for (size_t i = 0; i < m->function_count; i++) {
        add_item(items, m->functions[i].name, plum_native_is_methods(m) ? CK_METHOD : CK_FUNCTION,
                 m->functions[i].signature, m->functions[i].doc);
    }
    for (size_t i = 0; i < m->constant_count; i++) {
        add_item(items, m->constants[i].name, CK_CONSTANT,
                 plum_native_constant_type(&m->constants[i]), m->constants[i].doc);
    }
}

static int by_label(const void *x, const void *y) {
    return strcmp(((const Item *)x)->label, ((const Item *)y)->label);
}

/* Sort the items from `first` on by label (the seen map's indexes no
 * longer matter: nothing is added after). */
static void sort_items(Items *items, size_t first) {
    qsort(items->list.items + first, items->list.len - first, sizeof(Item), by_label);
}

/* What a module's `name.` offers: a standard module's functions and
 * constants, or a file's exported names. */
static void add_module_members(Items *items, const PlumLspAnalysis *a, const PlumSymbol *mod) {
    const PlumNativeModule *nm = native_module_of(mod);
    if (nm != NULL) {
        add_native_members(items, nm);
        return;
    }
    if (mod->decl == NULL || mod->decl->as.seed.exports == NULL) {
        return;
    }
    size_t cursor = 0;
    const PlumMapEntry *entry;
    while ((entry = plum_map_next(mod->decl->as.seed.exports, &cursor)) != NULL) {
        add_symbol(items, a, entry->value);
    }
    sort_items(items, 0); /* the map's order is its hash order */
}

/* The fields and methods of a tree, its parents and its traits. */
static void add_tree_members(Items *items, const PlumLspAnalysis *a, const PlumSymbol *tree,
                             int depth) {
    if (tree == NULL || depth > 32) {
        return;
    }
    if (tree->decl == NULL) {
        add_item_once(items, "message", CK_FIELD, "message: str", NULL); /* a built-in error */
        return;
    }
    const PlumStmt *s = tree->decl;
    if (s->kind != PLUM_STMT_TREE && s->kind != PLUM_STMT_TRAIT) {
        return;
    }
    const PlumModule *m = module_of(a, s, a->main);
    for (size_t i = 0; i < s->as.tree.field_count; i++) {
        const PlumField *f = &s->as.tree.fields[i];
        Buf d = {0};
        header_text(m, f->span.offset, span_end(f->span, m->len), &d);
        add_item_once(items, f->name, CK_FIELD, d.items, NULL);
        plum_array_free(&d);
    }
    for (size_t i = 0; i < s->as.tree.method_count; i++) {
        const PlumBranch *b = &s->as.tree.methods[i];
        Buf d = {0};
        header_text(m, b->span.offset, span_end(b->span, m->len), &d);
        add_item_once(items, b->name, CK_METHOD, d.items, NULL);
        plum_array_free(&d);
    }
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        const PlumType *super = s->as.tree.supers[i];
        if (super != NULL && super->kind == PLUM_TYPE_NAME) {
            add_tree_members(items, a, super->as.name.symbol, depth + 1);
        }
    }
}

static void add_collection_methods(Items *items, PlumMethodReceiver receiver) {
    for (int m = 0; m < PLUM_METHOD_COUNT; m++) {
        const char *name = plum_method_name((PlumMethod)m);
        if (strchr(name, ' ') == NULL && plum_method_find(receiver, name) == m) {
            add_item_once(items, name, CK_METHOD, NULL, NULL);
        }
    }
}

/* The members of a value of type `t`. */
static void add_type_members(Items *items, const PlumLspAnalysis *a, const PlumTy *t) {
    if (t != NULL && t->kind == PLUM_TY_OPTIONAL) {
        t = t->as.elem;
    }
    if (t == NULL) {
        return;
    }
    switch (t->kind) {
    case PLUM_TY_TREE:
    case PLUM_TY_TRAIT:
        add_tree_members(items, a, t->as.decl, 0);
        break;
    case PLUM_TY_LIST:
        add_collection_methods(items, PLUM_RECEIVER_LIST);
        break;
    case PLUM_TY_MAP:
        add_collection_methods(items, PLUM_RECEIVER_MAP);
        break;
    case PLUM_TY_SET:
        add_collection_methods(items, PLUM_RECEIVER_SET);
        break;
    case PLUM_TY_STR:
        add_native_members(items, &plum_module_str);
        break;
    case PLUM_TY_JSON:
        add_native_members(items, &plum_module_json_value);
        break;
    case PLUM_TY_HANDLE:
        add_native_members(items, plum_native_handle_methods(t->as.handle));
        break;
    case PLUM_TY_TASK:
        add_item(items, "wait", CK_METHOD, NULL, NULL);
        add_item(items, "done", CK_METHOD, NULL, NULL);
        break;
    case PLUM_TY_CHANNEL:
        add_item(items, "send", CK_METHOD, NULL, NULL);
        add_item(items, "receive", CK_METHOD, NULL, NULL);
        add_item(items, "close", CK_METHOD, NULL, NULL);
        break;
    default:
        break;
    }
}

/* The name the file is checked with, in place of the word being typed. */
#define PLACEHOLDER "plum_lsp_completion__"

static void member_visit(Walk *w, const Occ *o) {
    const PlumExpr **found = w->ctx;
    if (o->kind == OCC_EXPR && o->expr->kind == PLUM_EXPR_FIELD && o->expr->as.field.name != NULL &&
        strcmp(o->expr->as.field.name, PLACEHOLDER) == 0) {
        *found = o->expr;
    }
}

static void member_items(Items *items, const PlumLspAnalysis *a, const char *text, size_t len,
                         size_t word_start, size_t word_end, PlumModuleReader read,
                         void *read_ctx) {
    /* `math.` or `shapes.`: a module, known without checking again */
    size_t name_end = word_start - 1; /* the dot */
    if (name_end > 0 && text[name_end - 1] == '?') {
        name_end--;
    }
    size_t name_start = name_end;
    while (name_start > 0 && is_name_char(text[name_start - 1])) {
        name_start--;
    }
    if (name_start < name_end && (name_start == 0 || text[name_start - 1] != '.')) {
        const PlumSymbol *mod =
            find_module(&a->main->program, text + name_start, name_end - name_start);
        if (mod != NULL) {
            add_module_members(items, a, mod);
            return;
        }
    }
    /* Otherwise check the file again with a name after the dot, and see
     * what the value before it is. */
    size_t plen = strlen(PLACEHOLDER);
    size_t patched_len = len - (word_end - word_start) + plen;
    char *patched = plum_alloc(patched_len + 1);
    memcpy(patched, text, word_start);
    memcpy(patched + word_start, PLACEHOLDER, plen);
    memcpy(patched + word_start + plen, text + word_end, len - word_end);
    patched[patched_len] = '\0';
    PlumLspAnalysis *b = plum_lsp_analyze(a->main->path, patched, patched_len, read, read_ctx);
    plum_free(patched);
    const PlumExpr *field = NULL;
    Walk w = {b->main, member_visit, &field, 0};
    for (size_t i = 0; i < b->main->program.count; i++) {
        walk_stmt(&w, b->main->program.stmts[i]);
    }
    const PlumExpr *obj = field != NULL ? field->as.field.object : NULL;
    if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL &&
        obj->symbol->kind == PLUM_SYM_KIND && obj->symbol->decl != NULL) {
        const PlumStmt *k = obj->symbol->decl;
        const PlumModule *m = module_of(b, k, b->main);
        for (size_t i = 0; i < k->as.kind.case_count; i++) {
            Buf d = {0};
            case_text(m, k, &k->as.kind.cases[i], &d);
            add_item(items, k->as.kind.cases[i].name, CK_ENUM_MEMBER, d.items, NULL);
            plum_array_free(&d);
        }
    } else if (obj != NULL) {
        add_type_members(items, b, obj->ty);
    }
    plum_lsp_analysis_free(b); /* the items hold copies */
}

void plum_lsp_completion(PlumJsonWriter *w, const PlumLspAnalysis *a, const char *text, size_t len,
                         size_t offset, PlumModuleReader read, void *read_ctx) {
    if (offset > len) {
        offset = len;
    }
    size_t word_start = offset;
    while (word_start > 0 && is_name_char(text[word_start - 1])) {
        word_start--;
    }
    size_t word_end = offset;
    while (word_end < len && is_name_char(text[word_end])) {
        word_end++;
    }
    Items items;
    memset(&items, 0, sizeof items);
    plum_map_init(&items.seen);
    if (word_start > 0 && text[word_start - 1] == '.' &&
        !(word_start > 1 && text[word_start - 2] == '.')) {
        member_items(&items, a, text, len, word_start, word_end, read, read_ctx);
    } else {
        add_scope(&items, a, offset);
    }
    plum_jw_object(w);
    plum_jw_key(w, "isIncomplete");
    plum_jw_bool(w, false);
    plum_jw_key(w, "items");
    plum_jw_array(w);
    for (size_t i = 0; i < items.list.len; i++) {
        const Item *it = &items.list.items[i];
        plum_jw_object(w);
        plum_jw_key(w, "label");
        plum_jw_string(w, it->label);
        plum_jw_key(w, "kind");
        plum_jw_int(w, it->kind);
        if (it->detail != NULL) {
            plum_jw_key(w, "detail");
            plum_jw_string(w, it->detail);
        }
        if (it->doc != NULL && it->doc[0] != '\0') {
            plum_jw_key(w, "documentation");
            plum_jw_string(w, it->doc);
        }
        plum_jw_object_end(w);
    }
    plum_jw_array_end(w);
    plum_jw_object_end(w);
    for (size_t i = 0; i < items.list.len; i++) {
        plum_free(items.list.items[i].label);
        plum_free(items.list.items[i].detail);
    }
    plum_array_free(&items.list);
    plum_map_free(&items.seen);
}

/* ------------------------------------------------------------------ */
/* Signature help                                                      */
/* ------------------------------------------------------------------ */

/* The tokens of a text, lexed again: they skip strings and comments. */
typedef struct Lexed {
    PlumArena arena;
    PlumInterner names;
    PlumDiagList diags;
    PlumTokenList tokens;
    PlumCommentList comments;
} Lexed;

static void lex_text(Lexed *l, const char *path, const char *text, size_t len) {
    PlumSource source = {path, text, len};
    plum_arena_init(&l->arena);
    plum_interner_init(&l->names);
    plum_diag_init(&l->diags, &source);
    memset(&l->tokens, 0, sizeof l->tokens);
    memset(&l->comments, 0, sizeof l->comments);
    plum_lex_comments(&source, &l->arena, &l->names, &l->diags, &l->tokens, &l->comments);
}

static void lexed_free(Lexed *l) {
    plum_array_free(&l->tokens);
    plum_array_free(&l->comments);
    plum_diag_free(&l->diags);
    plum_interner_free(&l->names);
    plum_arena_free(&l->arena);
}

#define CALL_DEPTH 256

/* An open bracket before the offset. */
typedef struct Open {
    PlumTokenKind kind; /* LPAREN, LBRACKET, LBRACE or STRING_HEAD */
    size_t token;       /* its index */
    size_t commas;      /* commas in it so far */
    size_t arg;         /* the token starting its last argument */
} Open;

/* The call the offset is in the arguments of. */
typedef struct CallAt {
    size_t paren;        /* the '(' */
    size_t callee_end;   /* the end of the name before it */
    size_t commas;       /* the argument's index */
    const char *keyword; /* `name:` starting the argument, or NULL (interned in the Lexed) */
    char closers[CALL_DEPTH * 2 + 1]; /* what closes every bracket open at the offset */
} CallAt;

static bool call_at(const Lexed *l, size_t offset, CallAt *out) {
    Open stack[CALL_DEPTH];
    size_t depth = 0;
    const PlumToken *t = l->tokens.items;
    size_t n = l->tokens.len;
    size_t i = 0;
    for (; i < n && t[i].span.offset < offset && t[i].kind != PLUM_TOK_EOF; i++) {
        switch (t[i].kind) {
        case PLUM_TOK_LPAREN:
        case PLUM_TOK_LBRACKET:
        case PLUM_TOK_LBRACE:
        case PLUM_TOK_STRING_HEAD:
            if (depth < CALL_DEPTH) {
                Open o = {t[i].kind, i, 0, i + 1};
                stack[depth++] = o;
            }
            break;
        case PLUM_TOK_RPAREN:
        case PLUM_TOK_RBRACKET:
        case PLUM_TOK_RBRACE:
        case PLUM_TOK_STRING_TAIL:
            if (depth > 0) {
                depth--;
            }
            break;
        case PLUM_TOK_COMMA:
            if (depth > 0) {
                stack[depth - 1].commas++;
                stack[depth - 1].arg = i + 1;
            }
            break;
        default:
            break;
        }
    }
    /* the innermost '(' not inside a block or an interpolation */
    size_t k = depth;
    while (k > 0 && stack[k - 1].kind != PLUM_TOK_LPAREN) {
        if (stack[k - 1].kind == PLUM_TOK_LBRACE || stack[k - 1].kind == PLUM_TOK_STRING_HEAD) {
            return false;
        }
        k--;
    }
    if (k == 0) {
        return false;
    }
    const Open *o = &stack[k - 1];
    if (o->token == 0 || t[o->token - 1].kind != PLUM_TOK_IDENT) {
        return false; /* (a + b), or a call of something without a name */
    }
    out->paren = t[o->token].span.offset;
    out->callee_end = span_end(t[o->token - 1].span, (size_t)-1);
    out->commas = o->commas;
    out->keyword = NULL;
    if (o->arg + 1 < i && t[o->arg].kind == PLUM_TOK_IDENT &&
        t[o->arg + 1].kind == PLUM_TOK_COLON) {
        out->keyword = t[o->arg].as.name;
    }
    size_t c = 0;
    for (size_t d = depth; d > 0; d--) {
        switch (stack[d - 1].kind) {
        case PLUM_TOK_LPAREN:
            out->closers[c++] = ')';
            break;
        case PLUM_TOK_LBRACKET:
            out->closers[c++] = ']';
            break;
        case PLUM_TOK_LBRACE:
            out->closers[c++] = '}';
            break;
        default:
            out->closers[c++] = '}';
            out->closers[c++] = '"';
            break;
        }
    }
    out->closers[c] = '\0';
    return true;
}

typedef struct CallFinder {
    const CallAt *at;
    const PlumExpr *call;
} CallFinder;

static void call_visit(Walk *w, const Occ *o) {
    CallFinder *f = w->ctx;
    const PlumExpr *e = o->expr;
    if (o->kind == OCC_EXPR && e->kind == PLUM_EXPR_CALL && e->as.call.callee != NULL &&
        span_end(e->as.call.callee->span, w->m->len) == f->at->callee_end &&
        e->span.offset <= f->at->paren) {
        f->call = e;
    }
}

static const PlumExpr *find_call(const PlumModule *m, const CallAt *at) {
    CallFinder f = {at, NULL};
    Walk w = {m, call_visit, &f, 0};
    for (size_t i = 0; i < m->program.count; i++) {
        walk_stmt(&w, m->program.stmts[i]);
    }
    return f.call;
}

/* "branch area(r: float) -> float" without its "branch ". */
static void branch_label(const PlumModule *m, const PlumBranch *b, const char *name, Buf *out) {
    Buf h = {0};
    header_text(m, b->span.offset, span_end(b->span, m->len), &h);
    const char *text = buf_text(&h);
    const char *paren = strchr(text, '(');
    buf_str(out, name != NULL ? name : b->name);
    buf_str(out, paren != NULL ? paren : "()");
    plum_array_free(&h);
}

/* A tree's constructor: init's parameters, or every field, parents' first. */
static void constructor_label(const PlumLspAnalysis *a, const PlumSymbol *tree, Buf *out) {
    const PlumSymbol *owner = NULL;
    const PlumBranch *init = find_method(tree, "init", &owner, 0);
    if (init != NULL) {
        branch_label(module_of(a, owner->decl, a->main), init, tree->name, out);
        return;
    }
    const PlumSymbol *chain[32];
    size_t depth = 0;
    for (const PlumSymbol *t = tree; t != NULL && depth < 32; t = parent_tree(t)) {
        chain[depth++] = t;
    }
    buf_str(out, tree->name);
    buf_str(out, "(");
    bool first = true;
    for (size_t d = depth; d > 0; d--) {
        const PlumStmt *s = chain[d - 1]->decl;
        if (s == NULL) {
            buf_str(out, first ? "message: str" : ", message: str");
            first = false;
            continue;
        }
        const PlumModule *m = module_of(a, s, a->main);
        for (size_t i = 0; i < s->as.tree.field_count; i++) {
            if (!first) {
                buf_str(out, ", ");
            }
            header_text(m, s->as.tree.fields[i].span.offset,
                        span_end(s->as.tree.fields[i].span, m->len), out);
            first = false;
        }
    }
    buf_str(out, ")");
}

/* The signature of what a call calls, as a label like "f(a: int, b: str) -> bool",
 * and its documentation. False if it isn't known. */
static bool call_label(const PlumLspAnalysis *a, const PlumExpr *call, Buf *label,
                       const char **doc) {
    const PlumExpr *callee = call->as.call.callee;
    *doc = NULL;
    const PlumSymbol *kind = NULL;
    const char *case_name = NULL;
    if (callee->kind == PLUM_EXPR_NAME && callee->symbol != NULL) {
        const PlumSymbol *sym = callee->symbol;
        switch (sym->kind) {
        case PLUM_SYM_FUNC:
            if (sym->decl == NULL || sym->decl->kind != PLUM_STMT_BRANCH) {
                return false;
            }
            branch_label(module_of(a, sym->decl, a->main), &sym->decl->as.branch, NULL, label);
            return true;
        case PLUM_SYM_TREE:
            constructor_label(a, sym, label);
            return true;
        case PLUM_SYM_CASE:
            kind = sym->owner;
            case_name = sym->name;
            break;
        case PLUM_SYM_BUILTIN_FUNC:
            for (size_t i = 0; i < sizeof builtin_docs / sizeof builtin_docs[0]; i++) {
                if (strcmp(builtin_docs[i].name, sym->name) == 0) {
                    buf_str(label, builtin_docs[i].signature);
                    *doc = builtin_docs[i].doc;
                    return true;
                }
            }
            return false;
        case PLUM_SYM_NATIVE: {
            const PlumNativeModule *nm = native_module_of(sym);
            const PlumNativeFunction *fn = nm != NULL ? plum_native_function(nm, sym->name) : NULL;
            if (fn == NULL) {
                return false;
            }
            buf_str(label, sym->name);
            buf_str(label, fn->signature);
            *doc = fn->doc;
            return true;
        }
        default:
            return false;
        }
    } else if (callee->kind == PLUM_EXPR_FIELD && callee->as.field.name != NULL) {
        const PlumExpr *obj = callee->as.field.object;
        const char *name = callee->as.field.name;
        if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL &&
            obj->symbol->kind == PLUM_SYM_KIND) {
            kind = obj->symbol;
            case_name = name;
        } else {
            const PlumSymbol *tree = tree_of(obj != NULL ? obj->ty : NULL);
            const PlumSymbol *owner = NULL;
            const PlumBranch *m = tree != NULL ? find_method(tree, name, &owner, 0) : NULL;
            if (m != NULL) {
                branch_label(module_of(a, owner->decl, a->main), m, NULL, label);
                return true;
            }
            const PlumNativeFunction *fn = NULL;
            bool method = false;
            if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL &&
                obj->symbol->kind == PLUM_SYM_MODULE) {
                const PlumNativeModule *nm = native_module_of(obj->symbol);
                fn = nm != NULL ? plum_native_function(nm, name) : NULL;
            } else if (obj != NULL && obj->ty != NULL) {
                fn = plum_callee_native(callee, &method);
            }
            if (fn == NULL) {
                return false;
            }
            buf_str(label, name);
            const char *sig = fn->signature;
            if (method && strncmp(sig, "(self", 5) == 0) {
                /* "(self: str, old: str)": the receiver is before the dot */
                const char *comma = strchr(sig, ',');
                const char *close = strchr(sig, ')');
                buf_str(label, "(");
                sig = comma != NULL && (close == NULL || comma < close) ? comma + 1 : close;
                while (sig != NULL && *sig == ' ') {
                    sig++;
                }
                if (sig == NULL) {
                    sig = ")";
                }
            }
            buf_str(label, sig);
            *doc = fn->doc;
            return true;
        }
    } else {
        return false;
    }
    const PlumCase *c = kind != NULL ? find_case(kind, case_name) : NULL;
    if (c == NULL || c->param_count == 0) {
        return false;
    }
    case_text(module_of(a, kind->decl, a->main), kind->decl, c, label);
    return true;
}

/* The parameters in a label: [start, end) of each, between its first
 * parentheses, split at commas outside brackets and strings. */
typedef struct LabelParam {
    size_t start, end;
} LabelParam;

static size_t label_params(const char *label, LabelParam *out, size_t max) {
    const char *open = strchr(label, '(');
    if (open == NULL) {
        return 0;
    }
    size_t count = 0;
    int depth = 0;
    bool in_string = false;
    size_t start = (size_t)(open - label) + 1;
    for (size_t i = start; label[i] != '\0'; i++) {
        char c = label[i];
        if (in_string) {
            if (c == '\\' && label[i + 1] != '\0') {
                i++;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            depth--;
        } else if (depth == 0 && (c == ',' || c == ')')) {
            size_t s = start, e = i;
            while (s < e && label[s] == ' ') {
                s++;
            }
            while (e > s && label[e - 1] == ' ') {
                e--;
            }
            if (e > s && count < max) {
                out[count].start = s;
                out[count].end = e;
                count++;
            }
            start = i + 1;
            if (c == ')') {
                break;
            }
        }
    }
    return count;
}

/* The UTF-16 length of label[0, n). */
static long long utf16_at(const char *label, size_t n) {
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, label, strlen(label));
    long long line, character;
    plum_lsp_position(&lines, n, &line, &character);
    plum_lsp_lines_free(&lines);
    return character;
}

#define MAX_PARAMS 64

void plum_lsp_signature_help(PlumJsonWriter *w, const PlumLspAnalysis *a, const char *text,
                             size_t len, size_t offset, PlumModuleReader read, void *read_ctx) {
    if (offset > len) {
        offset = len;
    }
    Lexed l;
    lex_text(&l, a->main->path, text, len);
    CallAt at;
    if (!call_at(&l, offset, &at)) {
        lexed_free(&l);
        plum_jw_null(w);
        return;
    }
    const PlumLspAnalysis *in = a;
    PlumLspAnalysis *patched = NULL;
    const PlumExpr *call = find_call(a->main, &at);
    if (call == NULL) {
        /* A call being typed: check the text with its brackets closed
         * where the offset is, and the rest of its line left out. */
        size_t rest = offset;
        while (rest < len && text[rest] != '\n') {
            rest++;
        }
        size_t nc = strlen(at.closers);
        size_t plen = offset + nc + (len - rest);
        char *p = plum_alloc(plen + 1);
        memcpy(p, text, offset);
        memcpy(p + offset, at.closers, nc);
        memcpy(p + offset + nc, text + rest, len - rest);
        p[plen] = '\0';
        patched = plum_lsp_analyze(a->main->path, p, plen, read, read_ctx);
        plum_free(p);
        in = patched;
        call = find_call(patched->main, &at);
    }
    Buf label = {0};
    const char *doc = NULL;
    if (call == NULL || !call_label(in, call, &label, &doc)) {
        plum_array_free(&label);
        plum_lsp_analysis_free(patched);
        lexed_free(&l);
        plum_jw_null(w);
        return;
    }
    const char *text_label = buf_text(&label);
    LabelParam params[MAX_PARAMS];
    size_t count = label_params(text_label, params, MAX_PARAMS);
    size_t active = at.commas;
    if (at.keyword != NULL) {
        size_t kn = strlen(at.keyword);
        for (size_t i = 0; i < count; i++) {
            const char *p = text_label + params[i].start;
            if ((size_t)(params[i].end - params[i].start) >= kn && memcmp(p, at.keyword, kn) == 0 &&
                !is_name_char(p[kn])) {
                active = i;
                break;
            }
        }
    }
    /* drop(values...): every argument is the last parameter */
    if (count > 0 && active >= count && params[count - 1].end - params[count - 1].start > 3 &&
        memcmp(text_label + params[count - 1].end - 3, "...", 3) == 0) {
        active = count - 1;
    }
    plum_jw_object(w);
    plum_jw_key(w, "signatures");
    plum_jw_array(w);
    plum_jw_object(w);
    plum_jw_key(w, "label");
    plum_jw_string(w, text_label);
    if (doc != NULL && doc[0] != '\0') {
        plum_jw_key(w, "documentation");
        plum_jw_string(w, doc);
    }
    plum_jw_key(w, "parameters");
    plum_jw_array(w);
    for (size_t i = 0; i < count; i++) {
        plum_jw_object(w);
        plum_jw_key(w, "label");
        plum_jw_array(w);
        plum_jw_int(w, utf16_at(text_label, params[i].start));
        plum_jw_int(w, utf16_at(text_label, params[i].end));
        plum_jw_array_end(w);
        plum_jw_object_end(w);
    }
    plum_jw_array_end(w);
    plum_jw_key(w, "activeParameter");
    plum_jw_int(w, (long long)active);
    plum_jw_object_end(w);
    plum_jw_array_end(w);
    plum_jw_key(w, "activeSignature");
    plum_jw_int(w, 0);
    plum_jw_key(w, "activeParameter");
    plum_jw_int(w, (long long)active);
    plum_jw_object_end(w);
    plum_array_free(&label);
    plum_lsp_analysis_free(patched);
    lexed_free(&l);
}

/* ------------------------------------------------------------------ */
/* Semantic tokens                                                     */
/* ------------------------------------------------------------------ */

/* The legend: the protocol's standard token types and modifiers, in the
 * order their numbers refer to. */
enum {
    ST_NAMESPACE,   /* modules: seed math, seed ./shapes as sh */
    ST_TYPE,        /* built-in types: int, str, list */
    ST_CLASS,       /* trees, the built-in errors */
    ST_ENUM,        /* kinds */
    ST_INTERFACE,   /* traits */
    ST_PARAMETER,   /* parameters, keyword arguments */
    ST_VARIABLE,    /* plant, root, loop names, pattern names */
    ST_PROPERTY,    /* fields */
    ST_ENUM_MEMBER, /* cases */
    ST_FUNCTION,    /* branch, built-in and standard-library functions */
    ST_METHOD,      /* methods */
    ST_KEYWORD,
    ST_STRING,
    ST_NUMBER,
    ST_COMMENT,
    ST_NONE = -1,
};

enum {
    SM_DECLARATION = 1 << 0,
    SM_READONLY = 1 << 1,        /* root constants, standard-library constants */
    SM_DEFAULT_LIBRARY = 1 << 2, /* built into Plumlang */
};

static const char *const token_types[] = {
    "namespace",  "type",     "class",  "enum",    "interface", "parameter", "variable", "property",
    "enumMember", "function", "method", "keyword", "string",    "number",    "comment",
};

static const char *const token_modifiers[] = {"declaration", "readonly", "defaultLibrary"};

void plum_lsp_semantic_legend(PlumJsonWriter *w) {
    plum_jw_object(w);
    plum_jw_key(w, "tokenTypes");
    plum_jw_array(w);
    for (size_t i = 0; i < sizeof token_types / sizeof token_types[0]; i++) {
        plum_jw_string(w, token_types[i]);
    }
    plum_jw_array_end(w);
    plum_jw_key(w, "tokenModifiers");
    plum_jw_array(w);
    for (size_t i = 0; i < sizeof token_modifiers / sizeof token_modifiers[0]; i++) {
        plum_jw_string(w, token_modifiers[i]);
    }
    plum_jw_array_end(w);
    plum_jw_object_end(w);
}

typedef struct SemToken {
    size_t start, end;
    int type, mods;
} SemToken;

typedef PLUM_ARRAY(SemToken) SemTokens;

/* A token, cut at line ends (a token may not span lines). */
static void add_sem(SemTokens *out, const char *text, size_t start, size_t end, int type,
                    int mods) {
    while (start < end) {
        size_t stop = start;
        while (stop < end && text[stop] != '\n' && text[stop] != '\r') {
            stop++;
        }
        if (stop > start) {
            SemToken t = {start, stop, type, mods};
            plum_array_push(out, t);
        }
        start = stop + 1;
    }
}

static int symbol_token(const PlumSymbol *sym, int *mods) {
    switch (sym->kind) {
    case PLUM_SYM_VAR:
    case PLUM_SYM_LOOP:
    case PLUM_SYM_BINDING:
        return ST_VARIABLE;
    case PLUM_SYM_CONST:
        *mods |= SM_READONLY;
        return ST_VARIABLE;
    case PLUM_SYM_PARAM:
        return ST_PARAMETER;
    case PLUM_SYM_FUNC:
        return ST_FUNCTION;
    case PLUM_SYM_TREE:
        if (sym->decl == NULL) {
            *mods |= SM_DEFAULT_LIBRARY;
        }
        return ST_CLASS;
    case PLUM_SYM_TRAIT:
        return ST_INTERFACE;
    case PLUM_SYM_KIND:
        return ST_ENUM;
    case PLUM_SYM_CASE:
        return ST_ENUM_MEMBER;
    case PLUM_SYM_BUILTIN_FUNC:
        *mods |= SM_DEFAULT_LIBRARY;
        return ST_FUNCTION;
    case PLUM_SYM_BUILTIN_TYPE:
        *mods |= SM_DEFAULT_LIBRARY;
        return ST_TYPE;
    case PLUM_SYM_MODULE:
        if (native_module_of(sym) != NULL) {
            *mods |= SM_DEFAULT_LIBRARY;
        }
        return ST_NAMESPACE;
    case PLUM_SYM_NATIVE: {
        const PlumNativeModule *nm = native_module_of(sym);
        *mods |= SM_DEFAULT_LIBRARY;
        if (nm != NULL && plum_native_constant(nm, sym->name) != NULL) {
            *mods |= SM_READONLY;
            return ST_VARIABLE;
        }
        return ST_FUNCTION;
    }
    }
    return ST_NONE;
}

/* A standard module's function or constant. */
static int native_token(const PlumNativeModule *nm, const char *name, int *mods) {
    *mods |= SM_DEFAULT_LIBRARY;
    if (nm != NULL && plum_native_constant(nm, name) != NULL) {
        *mods |= SM_READONLY;
        return ST_VARIABLE;
    }
    return ST_FUNCTION;
}

static int member_token(const PlumExpr *e, int *mods) {
    const PlumExpr *obj = e->as.field.object;
    const char *name = e->as.field.name;
    if (obj != NULL && obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL) {
        if (obj->symbol->kind == PLUM_SYM_KIND) {
            return ST_ENUM_MEMBER;
        }
        const PlumNativeModule *nm =
            obj->symbol->kind == PLUM_SYM_MODULE ? native_module_of(obj->symbol) : NULL;
        if (nm != NULL) {
            return native_token(nm, name, mods);
        }
    }
    const PlumSymbol *tree = tree_of(obj != NULL ? obj->ty : NULL);
    const PlumSymbol *owner = NULL;
    if (tree != NULL && find_field(tree, name, &owner) != NULL) {
        return ST_PROPERTY;
    }
    if (tree != NULL && find_method(tree, name, &owner, 0) != NULL) {
        return ST_METHOD;
    }
    if (tree != NULL && tree->decl == NULL) {
        *mods |= SM_DEFAULT_LIBRARY; /* a built-in error's message */
        return ST_PROPERTY;
    }
    bool method = false;
    if (obj != NULL && obj->ty != NULL && plum_callee_native(e, &method) != NULL) {
        *mods |= SM_DEFAULT_LIBRARY;
        return ST_METHOD;
    }
    return e->ty != NULL && e->ty->kind == PLUM_TY_FUNC ? ST_METHOD : ST_PROPERTY;
}

typedef struct Sem {
    const PlumLspAnalysis *a;
    SemTokens *out;
} Sem;

static void sem_visit(Walk *w, const Occ *o) {
    Sem *s = w->ctx;
    int mods = 0;
    int type = ST_NONE;
    switch (o->kind) {
    case OCC_SYMBOL: {
        const PlumSymbol *sym = o->symbol;
        if (sym == NULL) {
            return;
        }
        type = symbol_token(sym, &mods);
        /* declared here? (cheap test first: is it inside the declaration) */
        Place p;
        if (sym->span.offset <= o->start && o->start < span_end(sym->span, (size_t)-1) &&
            symbol_place(s->a, sym, &p) == PLACE_OK && p.m == w->m && p.start == o->start) {
            mods |= SM_DECLARATION;
        }
        break;
    }
    case OCC_MEMBER:
        type = member_token(o->expr, &mods);
        break;
    case OCC_FIELD_DECL:
        type = ST_PROPERTY;
        mods = SM_DECLARATION;
        break;
    case OCC_METHOD_DECL:
        type = ST_METHOD;
        mods = SM_DECLARATION;
        break;
    case OCC_CASE_DECL:
        type = ST_ENUM_MEMBER;
        mods = SM_DECLARATION;
        break;
    case OCC_PARAM_DECL:
        type = ST_PARAMETER;
        mods = SM_DECLARATION;
        break;
    case OCC_SEED_PATH:
        type = ST_NAMESPACE;
        if (o->stmt->as.seed.exports == NULL) {
            mods = SM_DEFAULT_LIBRARY;
        }
        break;
    case OCC_SEED_NAME: {
        void *found = NULL;
        const PlumMap *exports = o->stmt->as.seed.exports;
        if (exports != NULL && plum_map_get(exports, o->name, strlen(o->name), &found)) {
            type = symbol_token(found, &mods);
        } else if (exports == NULL) {
            type = native_token(plum_native_module(o->stmt->as.seed.path), o->name, &mods);
        }
        break;
    }
    case OCC_ARG:
        type = ST_PARAMETER;
        break;
    case OCC_EXPR:
        return;
    }
    if (type != ST_NONE) {
        add_sem(s->out, w->m->text, o->start, o->end, type, mods);
    }
}

static int by_token(const void *x, const void *y) {
    const SemToken *a = x, *b = y;
    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    return a->end < b->end ? -1 : a->end > b->end;
}

void plum_lsp_semantic_tokens(PlumJsonWriter *w, const PlumLspAnalysis *a) {
    const PlumModule *m = a->main;
    SemTokens tokens = {0};
    /* keywords, numbers, strings and comments, from the lexer */
    Lexed l;
    lex_text(&l, m->path, m->text, m->len);
    for (size_t i = 0; i < l.tokens.len; i++) {
        const PlumToken *t = &l.tokens.items[i];
        size_t start = t->span.offset, end = span_end(t->span, m->len);
        int type = ST_NONE;
        if (t->kind >= PLUM_TOK_PLANT && t->kind <= PLUM_TOK_WEAK) {
            type = ST_KEYWORD;
        } else if (t->kind == PLUM_TOK_INT || t->kind == PLUM_TOK_FLOAT) {
            type = ST_NUMBER;
        } else if (t->kind == PLUM_TOK_STRING || t->kind == PLUM_TOK_STRING_HEAD ||
                   t->kind == PLUM_TOK_STRING_MID || t->kind == PLUM_TOK_STRING_TAIL) {
            type = ST_STRING;
        }
        if (type != ST_NONE) {
            add_sem(&tokens, m->text, start, end, type, 0);
        }
    }
    for (size_t i = 0; i < l.comments.len; i++) {
        const PlumComment *c = &l.comments.items[i];
        add_sem(&tokens, m->text, c->span.offset, span_end(c->span, m->len), ST_COMMENT, 0);
    }
    lexed_free(&l);
    /* names, by what they stand for */
    Sem sem = {a, &tokens};
    Walk walk = {m, sem_visit, &sem, 0};
    for (size_t i = 0; i < m->program.count; i++) {
        walk_stmt(&walk, m->program.stmts[i]);
    }
    if (tokens.len > 1) {
        qsort(tokens.items, tokens.len, sizeof *tokens.items, by_token);
    }
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, m->text, m->len);
    plum_jw_object(w);
    plum_jw_key(w, "data");
    plum_jw_array(w);
    long long prev_line = 0, prev_char = 0;
    size_t covered = 0; /* tokens may not overlap: a later one inside an earlier is left out */
    for (size_t i = 0; i < tokens.len; i++) {
        const SemToken *t = &tokens.items[i];
        if (t->start < covered || t->end > m->len) {
            continue;
        }
        covered = t->end;
        long long line, character, end_line, end_character;
        plum_lsp_position(&lines, t->start, &line, &character);
        plum_lsp_position(&lines, t->end, &end_line, &end_character);
        if (end_line != line || end_character <= character) {
            continue;
        }
        plum_jw_int(w, line - prev_line);
        plum_jw_int(w, line == prev_line ? character - prev_char : character);
        plum_jw_int(w, end_character - character);
        plum_jw_int(w, t->type);
        plum_jw_int(w, t->mods);
        prev_line = line;
        prev_char = character;
    }
    plum_jw_array_end(w);
    plum_jw_object_end(w);
    plum_lsp_lines_free(&lines);
    plum_array_free(&tokens);
}

/* ------------------------------------------------------------------ */
/* Code actions                                                        */
/* ------------------------------------------------------------------ */

/* The name a diagnostic's message quotes first: 'x' in "'x' is never used". */
static bool quoted_name(const char *message, char *out, size_t size) {
    const char *open = strchr(message, '\'');
    const char *close = open != NULL ? strchr(open + 1, '\'') : NULL;
    size_t n = close != NULL ? (size_t)(close - open - 1) : 0;
    if (n == 0 || n >= size) {
        return false;
    }
    memcpy(out, open + 1, n);
    out[n] = '\0';
    return true;
}

/* Where a new `seed` line goes: after the last one at the top level, or
 * else after the comments a file starts with. */
static size_t seed_line_at(const PlumModule *m) {
    size_t at = 0;
    bool any = false;
    for (size_t i = 0; i < m->program.count; i++) {
        const PlumStmt *s = m->program.stmts[i];
        if (s->kind == PLUM_STMT_SEED) {
            at = span_end(s->span, m->len);
            any = true;
        }
    }
    if (any) {
        while (at < m->len && m->text[at] != '\n') {
            at++;
        }
        return at < m->len ? at + 1 : at;
    }
    while (at < m->len && m->text[at] == '#') {
        while (at < m->len && m->text[at] != '\n') {
            at++;
        }
        if (at < m->len) {
            at++;
        }
    }
    return at;
}

/* A quick fix: one edit, inserting `text` at `at`, for diagnostic `d`. */
static void write_fix(PlumJsonWriter *w, const PlumModule *m, const PlumLspLines *lines,
                      const char *uri, const PlumDiag *d, const char *title, size_t at,
                      const char *text) {
    plum_jw_object(w);
    plum_jw_key(w, "title");
    plum_jw_string(w, title);
    plum_jw_key(w, "kind");
    plum_jw_string(w, "quickfix");
    plum_jw_key(w, "diagnostics");
    plum_jw_array(w);
    write_one_diagnostic(w, lines, m, d);
    plum_jw_array_end(w);
    plum_jw_key(w, "isPreferred");
    plum_jw_bool(w, true);
    plum_jw_key(w, "edit");
    plum_jw_object(w);
    plum_jw_key(w, "changes");
    plum_jw_object(w);
    plum_jw_key(w, uri);
    plum_jw_array(w);
    plum_jw_object(w);
    plum_jw_key(w, "range");
    write_range(w, lines, at, at);
    plum_jw_key(w, "newText");
    plum_jw_string(w, text);
    plum_jw_object_end(w);
    plum_jw_array_end(w);
    plum_jw_object_end(w);
    plum_jw_object_end(w);
    plum_jw_object_end(w);
}

static bool module_file_exists(const char *path, PlumModuleReader read, void *read_ctx) {
    size_t len = 0;
    int err = 0;
    char *text = read != NULL ? read(read_ctx, path, &len, &err) : plum_read_file(path, &len, &err);
    free(text); /* the readers' text is malloc'd */
    return text != NULL;
}

void plum_lsp_code_actions(PlumJsonWriter *w, const PlumLspAnalysis *a, const char *uri,
                           size_t start, size_t end, PlumModuleReader read, void *read_ctx) {
    const PlumModule *m = a->main;
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, m->text, m->len);
    plum_jw_array(w);
    for (size_t i = 0; i < m->diags.items.len; i++) {
        const PlumDiag *d = &m->diags.items.items[i];
        size_t from = d->span.offset, to = span_end(d->span, m->len);
        char name[128];
        if (from > end || to < start || !quoted_name(d->message, name, sizeof name)) {
            continue;
        }
        char title[192];
        if (strcmp(d->code, "W0502") == 0) {
            /* never used: `_x` keeps it without the warning */
            long at = find_word(m->text, m->len, from, to, name);
            if (at >= 0) {
                snprintf(title, sizeof title, "Prefix '%s' with '_' to keep it", name);
                write_fix(w, m, &lines, uri, d, title, (size_t)at, "_");
            }
        } else if (strcmp(d->code, "E0301") == 0) {
            /* an unknown name that is a module: seed it */
            const PlumNativeModule *nm = plum_native_module(name);
            char seed[160];
            const char *nl =
                seed_line_at(m) == m->len && m->len > 0 && m->text[m->len - 1] != '\n' ? "\n" : "";
            if (nm != NULL && !plum_native_is_methods(nm)) {
                snprintf(title, sizeof title, "Add `seed %s`", name);
                snprintf(seed, sizeof seed, "%sseed %s\n", nl, name);
                write_fix(w, m, &lines, uri, d, title, seed_line_at(m), seed);
                continue;
            }
            char rel[136];
            snprintf(rel, sizeof rel, "./%s", name);
            char *path = m->path[0] != '<' ? plum_module_path(m->path, rel) : NULL;
            if (path != NULL && !plum_lsp_same_path(path, m->path) &&
                module_file_exists(path, read, read_ctx)) {
                snprintf(title, sizeof title, "Add `seed ./%s`", name);
                snprintf(seed, sizeof seed, "%sseed ./%s\n", nl, name);
                write_fix(w, m, &lines, uri, d, title, seed_line_at(m), seed);
            }
            plum_free(path);
        }
    }
    plum_jw_array_end(w);
    plum_lsp_lines_free(&lines);
}
