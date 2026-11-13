/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "compiler/compiler.h"

#include "check/resolve.h"
#include "check/types.h"
#include "compiler/chunk.h"
#include "util/alloc.h"
#include "util/array.h"
#include "util/map.h"
#include "vm/methods.h"

#include <string.h>

#define NO_FORMAT 0xFFFF
#define MAX_SLOTS 0xFFFF

/* An enclosing loop: where continue goes, and jumps to patch at its end. */
/* A try statement being compiled: what leaving it early must undo. */
typedef struct Try {
    struct Try *outer;
    int handlers;            /* TRY instructions still active: 0 to 2 */
    const PlumStmt *finally; /* NULL without finally */
} Try;

typedef struct Loop {
    struct Loop *outer;
    Try *tries;   /* the tries around the loop; break and continue leave the ones inside */
    bool is_for;  /* continue jumps forward to the step (for) or back (while) */
    size_t start; /* while: where continue loops back to */
    PLUM_ARRAY(size_t) breaks;
    PLUM_ARRAY(size_t) continues; /* for loops: patched to the step */
} Loop;

/* The function being compiled. */
/* A variable a closure captures, and where it comes from. */
typedef struct Capture {
    PlumSymbol *sym;
    PlumCapture from;
} Capture;

typedef struct FnState {
    struct FnState *enclosing;
    PlumFunction *fn;
    int depth;     /* the resolver's function depth of its body */
    int next_slot; /* the next free local slot */
    Loop *loop;
    Try *tries;                   /* innermost first */
    PLUM_ARRAY(Capture) captures; /* the outer variables this function uses */
    PlumSymbol *self;             /* a nested function's own name: GET_SELF, not a capture */
} FnState;

typedef struct Compiler {
    PlumDiagList *diags;
    PlumInterner *names;
    PlumMap trees; /* tree name -> PlumTreeInfo*, one per tree for the whole program */
    FnState *fs;
    int global_count;
    PlumTy *result; /* the current function's result type, for return */
} Compiler;

static void compile_expr(Compiler *c, const PlumExpr *e);
static void compile_stmt(Compiler *c, const PlumStmt *s);

/* ------------------------------------------------------------------ */
/* Emitting                                                            */
/* ------------------------------------------------------------------ */

static PlumChunk *chunk(Compiler *c) {
    return c->fs->fn->chunk;
}

static void op(Compiler *c, PlumOpcode o, uint32_t line) {
    plum_chunk_write_op(chunk(c), o, line);
}

static void op1(Compiler *c, PlumOpcode o, uint16_t a, uint32_t line) {
    op(c, o, line);
    plum_chunk_write_u16(chunk(c), a, line);
}

static void op2(Compiler *c, PlumOpcode o, uint16_t a, uint16_t b, uint32_t line) {
    op1(c, o, a, line);
    plum_chunk_write_u16(chunk(c), b, line);
}

static void unsupported(Compiler *c, PlumSpan span, const char *what, int issue) {
    plum_diag_error(c->diags, span, "E0601",
                    "%s: not supported by the compiler yet (it arrives with #%d)", what, issue);
}

static uint16_t constant(Compiler *c, PlumValue v, PlumSpan span) {
    size_t i = plum_chunk_add_constant(chunk(c), v);
    if (i >= NO_FORMAT) {
        plum_diag_error(c->diags, span, "E0602",
                        "this function has more than 65535 constants; split it into smaller ones");
        return 0;
    }
    return (uint16_t)i;
}

static void emit_const(Compiler *c, PlumValue v, PlumSpan span) {
    op1(c, PLUM_OP_CONST, constant(c, v, span), span.line);
}

/* Emit a forward jump; returns where its operand is, for patching. */
static size_t emit_jump(Compiler *c, PlumOpcode o, uint32_t line) {
    op1(c, o, 0xFFFF, line);
    return chunk(c)->code.len - 2;
}

/* Point the jump whose operand is at `at` to the current position. */
static void patch_jump(Compiler *c, size_t at, PlumSpan span) {
    size_t distance = chunk(c)->code.len - (at + 2);
    if (distance > 0xFFFF) {
        plum_diag_error(c->diags, span, "E0602",
                        "this block is too long to jump over; split it into functions");
        return;
    }
    chunk(c)->code.items[at] = (uint8_t)(distance & 0xFF);
    chunk(c)->code.items[at + 1] = (uint8_t)(distance >> 8);
}

static void emit_loop(Compiler *c, size_t start, PlumSpan span) {
    size_t distance = chunk(c)->code.len + 3 - start;
    if (distance > 0xFFFF) {
        plum_diag_error(c->diags, span, "E0602", "this loop is too long; split it into functions");
        distance = 0;
    }
    op1(c, PLUM_OP_LOOP, (uint16_t)distance, span.line);
}

/* ------------------------------------------------------------------ */
/* Slots                                                               */
/* ------------------------------------------------------------------ */

static uint16_t new_local(Compiler *c, PlumSymbol *sym, PlumSpan span) {
    FnState *fs = c->fs;
    if (fs->next_slot >= MAX_SLOTS) {
        plum_diag_error(c->diags, span, "E0602",
                        "this function has more than 65535 local variables");
        return 0;
    }
    int slot = fs->next_slot++;
    if (fs->next_slot > fs->fn->slot_count) {
        fs->fn->slot_count = fs->next_slot;
    }
    if (sym != NULL) {
        sym->slot = slot;
        sym->has_slot = true;
    }
    return (uint16_t)slot;
}

/*
 * The index of `sym` among `fs`'s captures, adding it if needed: from the
 * enclosing function's cell when declared there, otherwise from what the
 * enclosing function itself captured.
 */
static int capture_index(FnState *fs, PlumSymbol *sym) {
    for (size_t i = 0; i < fs->captures.len; i++) {
        if (fs->captures.items[i].sym == sym) {
            return (int)i;
        }
    }
    FnState *outer = fs->enclosing;
    Capture cap = {sym, {true, 0}};
    if (outer->depth == sym->function_depth) {
        cap.from.index = (uint16_t)sym->slot;
    } else {
        cap.from.from_local = false;
        cap.from.index = (uint16_t)capture_index(outer, sym);
    }
    plum_array_push(&fs->captures, cap);
    return (int)fs->captures.len - 1;
}

/* Where a symbol lives right now: 0 a local slot, 1 a global, 2 a capture. */
static int symbol_place(Compiler *c, PlumSymbol *sym, uint16_t *index) {
    if (sym->global) {
        *index = (uint16_t)sym->slot;
        return 1;
    }
    if (sym->function_depth != c->fs->depth) {
        *index = (uint16_t)capture_index(c->fs, sym);
        return 2;
    }
    *index = (uint16_t)sym->slot;
    return 0;
}

static void load_symbol(Compiler *c, PlumSymbol *sym, PlumSpan span) {
    if (!sym->has_slot) {
        unsupported(c, span, "this kind of name", 22);
        return;
    }
    if (sym == c->fs->self) {
        /* Calling itself through a capture would make a cycle the closure and its
         * cell can't escape; the running function is on the stack. */
        op(c, PLUM_OP_GET_SELF, span.line);
        return;
    }
    uint16_t n;
    switch (symbol_place(c, sym, &n)) {
    case 1:
        op1(c, PLUM_OP_GET_GLOBAL, n, span.line);
        return;
    case 2:
        op1(c, PLUM_OP_GET_CAPTURE, n, span.line);
        return;
    default:
        op1(c, sym->captured ? PLUM_OP_GET_CELL : PLUM_OP_GET_LOCAL, n, span.line);
        return;
    }
}

static void store_symbol(Compiler *c, PlumSymbol *sym, PlumSpan span) {
    uint16_t n;
    switch (symbol_place(c, sym, &n)) {
    case 1:
        op1(c, PLUM_OP_SET_GLOBAL, n, span.line);
        return;
    case 2:
        op1(c, PLUM_OP_SET_CAPTURE, n, span.line);
        return;
    default:
        op1(c, sym->captured ? PLUM_OP_SET_CELL : PLUM_OP_SET_LOCAL, n, span.line);
        return;
    }
}

/* A new local of a captured variable goes in a cell, so closures share it. */
static void box_if_captured(Compiler *c, const PlumSymbol *sym, PlumSpan span) {
    if (sym != NULL && sym->captured && !sym->global) {
        op1(c, PLUM_OP_BOX_LOCAL, (uint16_t)sym->slot, span.line);
    }
}

/* An in-place instruction (method, index or path) on a variable: the local,
 * global or capture form, and its operand. */
static PlumOpcode place_op(Compiler *c, PlumSymbol *sym, PlumOpcode local, PlumOpcode global,
                           PlumOpcode capture, uint16_t *operand) {
    int kind = symbol_place(c, sym, operand);
    return kind == 0 ? local : kind == 1 ? global : capture;
}

/* ------------------------------------------------------------------ */
/* Types and conversions                                               */
/* ------------------------------------------------------------------ */

static bool ty_is(const PlumTy *t, PlumTyKind kind) {
    return t != NULL && t->kind == kind;
}

static bool int_like(const PlumTy *t) {
    return ty_is(t, PLUM_TY_INT) || ty_is(t, PLUM_TY_BYTE);
}

/* The checker allowed `from` where `to` is expected; convert if needed. */
static void widen(Compiler *c, const PlumTy *from, const PlumTy *to, uint32_t line) {
    if (to != NULL && to->kind == PLUM_TY_OPTIONAL) {
        to = to->as.elem;
    }
    if (int_like(from) && ty_is(to, PLUM_TY_FLOAT)) {
        op(c, PLUM_OP_INT_TO_FLOAT, line);
    }
}

/* Compile `e` as a value of type `to`. */
static void compile_as(Compiler *c, const PlumExpr *e, const PlumTy *to) {
    compile_expr(c, e);
    widen(c, e->ty, to, e->span.line);
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static PlumOpcode arith_op(PlumTokenKind tok, bool fl) {
    switch (tok) {
    case PLUM_TOK_PLUS:
        return fl ? PLUM_OP_ADD_FLOAT : PLUM_OP_ADD_INT;
    case PLUM_TOK_MINUS:
        return fl ? PLUM_OP_SUB_FLOAT : PLUM_OP_SUB_INT;
    case PLUM_TOK_STAR:
        return fl ? PLUM_OP_MUL_FLOAT : PLUM_OP_MUL_INT;
    case PLUM_TOK_SLASH:
        return PLUM_OP_DIV_FLOAT;
    case PLUM_TOK_SLASH_SLASH:
        return fl ? PLUM_OP_FLOORDIV_FLOAT : PLUM_OP_FLOORDIV_INT;
    case PLUM_TOK_PERCENT:
        return fl ? PLUM_OP_MOD_FLOAT : PLUM_OP_MOD_INT;
    case PLUM_TOK_STAR_STAR:
        return fl ? PLUM_OP_POW_FLOAT : PLUM_OP_POW_INT;
    case PLUM_TOK_AMP:
        return PLUM_OP_BAND;
    case PLUM_TOK_PIPE:
        return PLUM_OP_BOR;
    case PLUM_TOK_CARET:
        return PLUM_OP_BXOR;
    case PLUM_TOK_LESS_LESS:
        return PLUM_OP_SHL;
    default:
        return PLUM_OP_SHR;
    }
}

static PlumOpcode compare_op(PlumTokenKind tok, int kind /* 0 int, 1 float, 2 str */) {
    static const PlumOpcode table[4][3] = {
        {PLUM_OP_LT_INT, PLUM_OP_LT_FLOAT, PLUM_OP_LT_STR},
        {PLUM_OP_LE_INT, PLUM_OP_LE_FLOAT, PLUM_OP_LE_STR},
        {PLUM_OP_GT_INT, PLUM_OP_GT_FLOAT, PLUM_OP_GT_STR},
        {PLUM_OP_GE_INT, PLUM_OP_GE_FLOAT, PLUM_OP_GE_STR},
    };
    int row = tok == PLUM_TOK_LESS      ? 0
              : tok == PLUM_TOK_LESS_EQ ? 1
              : tok == PLUM_TOK_GREATER ? 2
                                        : 3;
    return table[row][kind];
}

/*
 * left op right for arithmetic and bitwise operators (also compound
 * assignment): both operands are converted to the result's type as they
 * are computed. `left` is NULL when the left value is already pushed.
 */
static void arithmetic(Compiler *c, PlumTokenKind tok, const PlumExpr *left, const PlumTy *lty,
                       const PlumExpr *right, uint32_t line) {
    bool fl = tok == PLUM_TOK_SLASH || ty_is(lty, PLUM_TY_FLOAT) || ty_is(right->ty, PLUM_TY_FLOAT);
    if (left != NULL) {
        compile_expr(c, left);
    }
    if (fl && int_like(lty)) {
        op(c, PLUM_OP_INT_TO_FLOAT, line);
    }
    compile_expr(c, right);
    if (fl && int_like(right->ty)) {
        op(c, PLUM_OP_INT_TO_FLOAT, line);
    }
    if (tok == PLUM_TOK_PLUS && ty_is(lty, PLUM_TY_STR)) {
        op(c, PLUM_OP_CONCAT, line);
        return;
    }
    if (tok == PLUM_TOK_PLUS && ty_is(lty, PLUM_TY_LIST)) {
        op(c, PLUM_OP_CONCAT_LIST, line);
        return;
    }
    op(c, arith_op(tok, fl), line);
}

static void compile_binary(Compiler *c, const PlumExpr *e) {
    PlumTokenKind tok = e->as.binary.op;
    const PlumExpr *l = e->as.binary.left, *r = e->as.binary.right;
    uint32_t line = e->span.line;

    switch (tok) {
    case PLUM_TOK_AND:
    case PLUM_TOK_OR: {
        compile_expr(c, l);
        size_t skip = emit_jump(
            c, tok == PLUM_TOK_AND ? PLUM_OP_JUMP_IF_FALSE_KEEP : PLUM_OP_JUMP_IF_TRUE_KEEP, line);
        op(c, PLUM_OP_POP, line);
        compile_expr(c, r);
        patch_jump(c, skip, e->span);
        return;
    }
    case PLUM_TOK_QUESTION_QUESTION: {
        compile_expr(c, l);
        size_t skip = emit_jump(c, PLUM_OP_JUMP_IF_NOT_NONE_KEEP, line);
        op(c, PLUM_OP_POP, line);
        compile_as(c, r, e->ty);
        patch_jump(c, skip, e->span);
        return;
    }
    case PLUM_TOK_EQ_EQ:
    case PLUM_TOK_BANG_EQ: {
        /* Numbers compare with a typed instruction; anything else, generically. */
        bool eq = tok == PLUM_TOK_EQ_EQ;
        bool ints = int_like(l->ty) && int_like(r->ty);
        bool floats = !ints && (int_like(l->ty) || ty_is(l->ty, PLUM_TY_FLOAT)) &&
                      (int_like(r->ty) || ty_is(r->ty, PLUM_TY_FLOAT));
        compile_expr(c, l);
        if (floats && int_like(l->ty)) {
            op(c, PLUM_OP_INT_TO_FLOAT, line);
        }
        compile_expr(c, r);
        if (floats && int_like(r->ty)) {
            op(c, PLUM_OP_INT_TO_FLOAT, line);
        }
        PlumOpcode o = ints     ? (eq ? PLUM_OP_EQ_INT : PLUM_OP_NE_INT)
                       : floats ? (eq ? PLUM_OP_EQ_FLOAT : PLUM_OP_NE_FLOAT)
                                : (eq ? PLUM_OP_EQ : PLUM_OP_NE);
        op(c, o, line);
        return;
    }
    case PLUM_TOK_LESS:
    case PLUM_TOK_LESS_EQ:
    case PLUM_TOK_GREATER:
    case PLUM_TOK_GREATER_EQ: {
        int kind = ty_is(l->ty, PLUM_TY_STR)                                    ? 2
                   : ty_is(l->ty, PLUM_TY_FLOAT) || ty_is(r->ty, PLUM_TY_FLOAT) ? 1
                                                                                : 0;
        compile_expr(c, l);
        if (kind == 1 && int_like(l->ty)) {
            op(c, PLUM_OP_INT_TO_FLOAT, line);
        }
        compile_expr(c, r);
        if (kind == 1 && int_like(r->ty)) {
            op(c, PLUM_OP_INT_TO_FLOAT, line);
        }
        op(c, compare_op(tok, kind), line);
        return;
    }
    case PLUM_TOK_IN:
        if (r->kind == PLUM_EXPR_RANGE) {
            /* x in a..b: a <= x and x < b (or <= b), with x computed once */
            int saved = c->fs->next_slot;
            uint16_t x = new_local(c, NULL, e->span);
            compile_expr(c, l);
            op1(c, PLUM_OP_SET_LOCAL, x, line);
            op1(c, PLUM_OP_GET_LOCAL, x, line);
            compile_expr(c, r->as.range.low);
            op(c, PLUM_OP_GE_INT, line);
            size_t skip = emit_jump(c, PLUM_OP_JUMP_IF_FALSE_KEEP, line);
            op(c, PLUM_OP_POP, line);
            op1(c, PLUM_OP_GET_LOCAL, x, line);
            compile_expr(c, r->as.range.high);
            op(c, r->as.range.inclusive ? PLUM_OP_LE_INT : PLUM_OP_LT_INT, line);
            patch_jump(c, skip, e->span);
            c->fs->next_slot = saved;
            return;
        }
        compile_as(c, l, ty_is(r->ty, PLUM_TY_MAP) ? r->ty->as.map.key : r->ty->as.elem);
        compile_expr(c, r);
        op(c, PLUM_OP_CONTAINS, line);
        return;
    default:
        arithmetic(c, tok, l, l->ty, r, line);
        return;
    }
}

static void compile_interp(Compiler *c, const PlumExpr *e) {
    size_t pieces = 0;
    for (size_t i = 0; i < e->as.interp.count; i++) {
        const PlumInterpPart *part = &e->as.interp.parts[i];
        if (part->expr == NULL) {
            emit_const(c, plum_obj(&plum_string_new(part->text, part->len)->obj), e->span);
        } else {
            compile_expr(c, part->expr);
            uint16_t fmt = NO_FORMAT;
            if (part->format != NULL) {
                fmt =
                    constant(c, plum_obj(&plum_string_new(part->format, strlen(part->format))->obj),
                             e->span);
            }
            op1(c, PLUM_OP_TO_STRING, fmt, part->expr->span.line);
        }
        pieces++;
    }
    if (pieces != 1) {
        op1(c, PLUM_OP_BUILD_STRING, (uint16_t)pieces, e->span.line);
    }
}

static const PlumBranch *callee_decl(const PlumSymbol *sym) {
    if (sym == NULL || sym->kind != PLUM_SYM_FUNC || sym->decl == NULL ||
        sym->decl->kind != PLUM_STMT_BRANCH) {
        return NULL;
    }
    return &sym->decl->as.branch;
}

/* Arguments in parameter order: positional, named, then defaults. */
static void compile_args(Compiler *c, const PlumExpr *call, const PlumTy *fty,
                         const PlumBranch *decl) {
    size_t n = fty->as.func.param_count;
    const PlumExpr *by_param[256] = {NULL};
    if (n > 255) {
        unsupported(c, call->span, "functions with more than 255 parameters", 36);
        return;
    }
    size_t positional = 0;
    for (size_t i = 0; i < call->as.call.arg_count; i++) {
        const PlumArg *arg = &call->as.call.args[i];
        size_t slot = positional;
        if (arg->name != NULL) {
            for (size_t k = 0; k < n; k++) {
                if (fty->as.func.params[k].name == arg->name) {
                    slot = k;
                }
            }
        } else {
            positional++;
        }
        if (slot < n) {
            by_param[slot] = arg->value;
        }
    }
    for (size_t k = 0; k < n; k++) {
        const PlumTy *want = fty->as.func.params[k].ty;
        if (by_param[k] != NULL) {
            compile_as(c, by_param[k], want);
        } else if (decl != NULL && decl->params[k].fallback != NULL) {
            compile_as(c, decl->params[k].fallback, want);
        } else {
            op(c, PLUM_OP_NONE, call->span.line); /* the checker reported it */
        }
    }
}

static int builtin_id(const char *name) {
    for (int i = 0; i < PLUM_BUILTIN_COUNT; i++) {
        if (strcmp(plum_builtin_name((PlumBuiltin)i), name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Trees                                                               */
/* ------------------------------------------------------------------ */

/* A field of a tree, parents' first; `decl` is NULL for an error's message. */
typedef struct FieldSlot {
    const char *name;
    const PlumField *decl;
} FieldSlot;

static PlumSymbol *parent_of(const PlumSymbol *tree) {
    if (tree->decl == NULL || tree->decl->kind != PLUM_STMT_TREE) {
        return NULL;
    }
    const PlumStmt *s = tree->decl;
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        const PlumType *super = s->as.tree.supers[i];
        if (super->kind == PLUM_TYPE_NAME && super->as.name.symbol != NULL &&
            super->as.name.symbol->kind == PLUM_SYM_TREE) {
            return super->as.name.symbol;
        }
    }
    return NULL;
}

static bool is_builtin_error(const PlumSymbol *tree) {
    return tree->kind == PLUM_SYM_TREE && tree->decl == NULL;
}

/* Every field of `tree` in object order: the parents' first. */
static void tree_fields(const PlumSymbol *tree, FieldSlot *out, int *count, int max) {
    if (tree == NULL || *count >= max) {
        return;
    }
    if (is_builtin_error(tree)) {
        if (*count < max) {
            out[(*count)++] = (FieldSlot){"message", NULL}; /* every error has a message */
        }
        return;
    }
    tree_fields(parent_of(tree), out, count, max);
    const PlumStmt *s = tree->decl;
    for (size_t i = 0; i < s->as.tree.field_count && *count < max; i++) {
        out[(*count)++] = (FieldSlot){s->as.tree.fields[i].name, &s->as.tree.fields[i]};
    }
}

#define MAX_FIELDS 256

static int field_index(const PlumSymbol *tree, const char *name) {
    FieldSlot fields[MAX_FIELDS];
    int n = 0;
    tree_fields(tree, fields, &n, MAX_FIELDS);
    for (int i = 0; i < n; i++) {
        if (strcmp(fields[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static PlumTreeInfo *tree_info(Compiler *c, PlumSymbol *tree);

/* The info of a built-in error tree, by name. */
static PlumTreeInfo *builtin_error_info(Compiler *c, const char *name) {
    PlumSymbol sym;
    memset(&sym, 0, sizeof sym);
    sym.kind = PLUM_SYM_TREE;
    sym.name = plum_intern_cstr(c->names, name);
    return tree_info(c, &sym);
}

/* The one PlumTreeInfo for `tree` in this program, made with its parents. */
static PlumTreeInfo *tree_info(Compiler *c, PlumSymbol *tree) {
    void *found = NULL;
    if (plum_map_get(&c->trees, tree->name, strlen(tree->name), &found)) {
        return found;
    }
    PlumTreeInfo *parent = NULL;
    if (is_builtin_error(tree)) {
        if (strcmp(tree->name, "Error") != 0) {
            /* ValueError, KeyError, ... all extend Error */
            parent = builtin_error_info(c, "Error");
        }
    } else if (parent_of(tree) != NULL) {
        parent = tree_info(c, parent_of(tree));
    }
    FieldSlot fields[MAX_FIELDS];
    const char *names[MAX_FIELDS];
    int n = 0;
    tree_fields(tree, fields, &n, MAX_FIELDS);
    for (int i = 0; i < n; i++) {
        names[i] = fields[i].name;
    }
    PlumTreeInfo *info = plum_tree_info_new(tree->name, parent, n, names);
    plum_map_set(&c->trees, tree->name, strlen(tree->name), info);
    return info;
}

/* Push a reference to `tree`'s info as a constant of the current chunk. */
static uint16_t tree_constant(Compiler *c, PlumSymbol *tree, PlumSpan span) {
    PlumTreeInfo *info = tree_info(c, tree);
    return constant(c, plum_retain(plum_obj(&info->obj)), span);
}

/* Does the field's declared type say float (so an int value widens)? */
static bool declared_float(const PlumType *t) {
    if (t != NULL && t->kind == PLUM_TYPE_OPTIONAL) {
        t = t->as.optional;
    }
    return t != NULL && t->kind == PLUM_TYPE_NAME && strcmp(t->as.name.name, "float") == 0;
}

/* Tree(args): the fields in order, by position or name, else their defaults. */
static void compile_constructor(Compiler *c, const PlumExpr *e, PlumSymbol *tree) {
    FieldSlot fields[MAX_FIELDS];
    int n = 0;
    tree_fields(tree, fields, &n, MAX_FIELDS);
    const PlumExpr *given[MAX_FIELDS] = {NULL};
    int positional = 0;
    for (size_t i = 0; i < e->as.call.arg_count; i++) {
        const PlumArg *arg = &e->as.call.args[i];
        int at = arg->name != NULL ? -1 : positional++;
        for (int k = 0; arg->name != NULL && k < n; k++) {
            if (strcmp(fields[k].name, arg->name) == 0) {
                at = k;
            }
        }
        if (at >= 0 && at < n) {
            given[at] = arg->value;
        }
    }
    for (int k = 0; k < n; k++) {
        const PlumExpr *v = given[k];
        if (v == NULL && fields[k].decl != NULL) {
            v = fields[k].decl->fallback;
        }
        if (v == NULL) {
            /* an error's message defaults to empty */
            emit_const(c, plum_obj(&plum_string_new("", 0)->obj), e->span);
            continue;
        }
        compile_expr(c, v);
        if (fields[k].decl != NULL && declared_float(fields[k].decl->type) && int_like(v->ty)) {
            op(c, PLUM_OP_INT_TO_FLOAT, e->span.line);
        }
    }
    op2(c, PLUM_OP_NEW_OBJECT, tree_constant(c, tree, e->span), (uint16_t)n, e->span.line);
}

/* The tree an expression's type is an object of (through T?), or NULL. */
static PlumSymbol *tree_of(const PlumTy *t) {
    if (t != NULL && t->kind == PLUM_TY_OPTIONAL) {
        t = t->as.elem;
    }
    return t != NULL && t->kind == PLUM_TY_TREE ? t->as.decl : NULL;
}

/* obj.field and obj?.field */
static void compile_field(Compiler *c, const PlumExpr *e) {
    const PlumExpr *obj = e->as.field.object;
    PlumSymbol *tree = tree_of(obj->ty);
    int index = tree != NULL ? field_index(tree, e->as.field.name) : -1;
    if (index < 0) {
        unsupported(c, e->span, "methods and kind cases", 22);
        return;
    }
    compile_expr(c, obj);
    size_t skip = 0;
    if (e->as.field.optional) {
        skip = emit_jump(c, PLUM_OP_JUMP_IF_NONE_KEEP, e->span.line); /* none?.x is none */
    }
    op1(c, PLUM_OP_GET_FIELD, (uint16_t)index, e->span.line);
    if (e->as.field.optional) {
        patch_jump(c, skip, e->span);
    }
}

static void compile_list(Compiler *c, const PlumExpr *e) {
    const PlumTy *elem = ty_is(e->ty, PLUM_TY_LIST) ? e->ty->as.elem : NULL;
    if (e->as.list.count > 0xFFFF) {
        plum_diag_error(c->diags, e->span, "E0602",
                        "a list literal can hold at most 65535 items; build it in a loop");
        return;
    }
    for (size_t i = 0; i < e->as.list.count; i++) {
        compile_as(c, e->as.list.items[i], elem);
    }
    op1(c, PLUM_OP_BUILD_LIST, (uint16_t)e->as.list.count, e->span.line);
}

static void compile_map(Compiler *c, const PlumExpr *e) {
    const PlumTy *k = ty_is(e->ty, PLUM_TY_MAP) ? e->ty->as.map.key : NULL;
    const PlumTy *v = ty_is(e->ty, PLUM_TY_MAP) ? e->ty->as.map.value : NULL;
    if (e->as.map.count > 0xFFFF) {
        plum_diag_error(c->diags, e->span, "E0602",
                        "a map literal can hold at most 65535 entries; build it in a loop");
        return;
    }
    for (size_t i = 0; i < e->as.map.count; i++) {
        compile_as(c, e->as.map.entries[i].key, k);
        compile_as(c, e->as.map.entries[i].value, v);
    }
    op1(c, PLUM_OP_BUILD_MAP, (uint16_t)e->as.map.count, e->span.line);
}

static void compile_index(Compiler *c, const PlumExpr *e) {
    const PlumExpr *obj = e->as.index.object, *idx = e->as.index.index;
    if (ty_is(obj->ty, PLUM_TY_MAP)) {
        compile_expr(c, obj);
        compile_as(c, idx, obj->ty->as.map.key);
        op(c, PLUM_OP_INDEX_MAP, e->span.line);
        return;
    }
    compile_expr(c, obj);
    if (idx->kind == PLUM_EXPR_RANGE) {
        compile_expr(c, idx->as.range.low);
        compile_expr(c, idx->as.range.high);
        op1(c, PLUM_OP_SLICE, idx->as.range.inclusive, e->span.line);
        return;
    }
    compile_expr(c, idx);
    op(c, ty_is(obj->ty, PLUM_TY_STR) ? PLUM_OP_INDEX_STR : PLUM_OP_INDEX_LIST, e->span.line);
}

/* A variable a method can change in place, or NULL. */
static PlumSymbol *place_of(Compiler *c, const PlumExpr *obj) {
    (void)c; /* any variable, here or captured: place_op picks the instruction */
    if (obj->kind != PLUM_EXPR_NAME || obj->symbol == NULL || !obj->symbol->has_slot) {
        return NULL;
    }
    return obj->symbol;
}

#define MAX_PATH 32

/*
 * grid[i][j]: a variable and the keys down from it, outermost first, when
 * every level is a list or map index (not a slice). NULL otherwise.
 */
static PlumSymbol *path_of(Compiler *c, const PlumExpr *e, const PlumExpr **keys, int *depth) {
    const PlumExpr *chain[MAX_PATH];
    int n = 0;
    while (e->kind == PLUM_EXPR_INDEX) {
        const PlumExpr *obj = e->as.index.object;
        if (n == MAX_PATH || e->as.index.index->kind == PLUM_EXPR_RANGE ||
            (!ty_is(obj->ty, PLUM_TY_LIST) && !ty_is(obj->ty, PLUM_TY_MAP))) {
            return NULL;
        }
        chain[n++] = e;
        e = obj;
    }
    PlumSymbol *base = place_of(c, e);
    if (base == NULL) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        keys[i] = chain[n - 1 - i]; /* the INDEX nodes, outermost first */
    }
    *depth = n;
    return base;
}

/* Push one path key, as a map key or a list position. */
static void compile_key(Compiler *c, const PlumExpr *index_node) {
    const PlumExpr *obj = index_node->as.index.object;
    if (ty_is(obj->ty, PLUM_TY_MAP)) {
        compile_as(c, index_node->as.index.index, obj->ty->as.map.key);
    } else {
        compile_expr(c, index_node->as.index.index);
    }
}

/* xs.method(args): in place on a variable, or on a temporary value. */
static void compile_method(Compiler *c, const PlumExpr *e) {
    const PlumExpr *callee = e->as.call.callee;
    const PlumExpr *obj = callee->as.field.object;
    const PlumTy *fty = callee->ty;
    const PlumTy *rty = obj->ty;
    PlumMethodReceiver receiver = ty_is(rty, PLUM_TY_MAP)   ? PLUM_RECEIVER_MAP
                                  : ty_is(rty, PLUM_TY_SET) ? PLUM_RECEIVER_SET
                                                            : PLUM_RECEIVER_LIST;
    int m = plum_method_find(receiver, callee->as.field.name);
    if (m < 0 || !ty_is(fty, PLUM_TY_FUNC) || callee->as.field.optional) {
        unsupported(c, e->span, "this method", 24);
        return;
    }
    uint32_t line = e->span.line;
    uint16_t argc = (uint16_t)fty->as.func.param_count;
    PlumSymbol *place = plum_method_mutates((PlumMethod)m) ? place_of(c, obj) : NULL;
    if (place == NULL && plum_method_mutates((PlumMethod)m) && obj->kind == PLUM_EXPR_INDEX) {
        /* m["k"].push(x): change the item inside the variable in place */
        const PlumExpr *keys[MAX_PATH];
        int depth = 0;
        PlumSymbol *base = path_of(c, obj, keys, &depth);
        if (base != NULL && argc < 256) {
            for (int i = 0; i < depth; i++) {
                compile_key(c, keys[i]);
            }
            compile_args(c, e, fty, NULL);
            uint16_t n;
            PlumOpcode o = place_op(c, base, PLUM_OP_METHOD_PATH_LOCAL, PLUM_OP_METHOD_PATH_GLOBAL,
                                    PLUM_OP_METHOD_PATH_CAPTURE, &n);
            op2(c, o, n, (uint16_t)m, line);
            plum_chunk_write_u16(chunk(c), (uint16_t)((depth << 8) | argc), line);
            return;
        }
    }
    if (place == NULL) {
        compile_expr(c, obj);
    }
    compile_args(c, e, fty, NULL);
    if (place == NULL) {
        op2(c, PLUM_OP_METHOD, (uint16_t)m, argc, line);
        return;
    }
    uint16_t n;
    PlumOpcode o =
        place_op(c, place, PLUM_OP_METHOD_LOCAL, PLUM_OP_METHOD_GLOBAL, PLUM_OP_METHOD_CAPTURE, &n);
    op2(c, o, n, (uint16_t)m, line);
    plum_chunk_write_u16(chunk(c), argc, line);
}

static void compile_call(Compiler *c, const PlumExpr *e) {
    const PlumExpr *callee = e->as.call.callee;
    PlumSymbol *sym = callee->kind == PLUM_EXPR_NAME ? callee->symbol : NULL;
    uint32_t line = e->span.line;

    if (sym != NULL && (sym->kind == PLUM_SYM_BUILTIN_FUNC || sym->kind == PLUM_SYM_BUILTIN_TYPE)) {
        int id = builtin_id(sym->name);
        if (id < 0) {
            unsupported(c, e->span, "this built-in", 24);
            return;
        }
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            compile_expr(c, e->as.call.args[i].value);
        }
        op2(c, PLUM_OP_CALL_BUILTIN, (uint16_t)id, (uint16_t)e->as.call.arg_count, line);
        return;
    }
    if (sym != NULL && sym->kind == PLUM_SYM_TREE) {
        if (!is_builtin_error(sym) && sym->decl != NULL) {
            const PlumStmt *d = sym->decl;
            for (size_t i = 0; i < d->as.tree.method_count; i++) {
                if (strcmp(d->as.tree.methods[i].name, "init") == 0) {
                    unsupported(c, e->span, "init constructors", 22);
                    return;
                }
            }
        }
        compile_constructor(c, e, sym);
        return;
    }
    if (sym != NULL && sym->kind == PLUM_SYM_KIND) {
        unsupported(c, e->span, "kinds", 22);
        return;
    }
    if (callee->kind == PLUM_EXPR_FIELD) {
        const PlumTy *rty = callee->as.field.object->ty;
        if (ty_is(rty, PLUM_TY_LIST) || ty_is(rty, PLUM_TY_MAP) || ty_is(rty, PLUM_TY_SET)) {
            compile_method(c, e);
        } else {
            unsupported(c, e->span, "methods", 22);
        }
        return;
    }
    const PlumTy *fty = callee->ty;
    if (!ty_is(fty, PLUM_TY_FUNC)) {
        unsupported(c, e->span, "this kind of call", 19);
        return;
    }
    compile_expr(c, callee);
    compile_args(c, e, fty, callee_decl(sym));
    op1(c, PLUM_OP_CALL, (uint16_t)fty->as.func.param_count, line);
}

static void compile_function(Compiler *c, PlumFunction *fn, const PlumBranch *b, int depth,
                             const PlumTy *result, PlumSymbol *self);
static void compile_function_body(Compiler *c, PlumFunction *fn, const PlumParam *params,
                                  size_t param_count, const PlumExpr *arrow, const PlumStmt *body,
                                  int depth, const PlumTy *result, PlumSymbol *self);

/* Push `fn` as a value: a plain function, or a closure if it captures. */
static void emit_function_value(Compiler *c, PlumFunction *fn, PlumSpan span) {
    bool closure = fn->capture_count > 0;
    uint16_t k = constant(c, plum_obj(&fn->obj), span);
    op1(c, closure ? PLUM_OP_MAKE_CLOSURE : PLUM_OP_CONST, k, span.line);
}

static void compile_lambda(Compiler *c, const PlumExpr *e) {
    char name[32];
    snprintf(name, sizeof name, "<lambda:%u>", (unsigned)e->span.line);
    const char *interned = plum_intern_cstr(c->names, name);
    PlumFunction *fn = plum_function_new(interned, (int)e->as.lambda.param_count);
    const PlumTy *result = ty_is(e->ty, PLUM_TY_FUNC) ? e->ty->as.func.result : NULL;
    plum_retain(plum_obj(&fn->obj));
    compile_function_body(c, fn, e->as.lambda.params, e->as.lambda.param_count, e->as.lambda.body,
                          e->as.lambda.block, c->fs->depth + 1, result, NULL);
    emit_function_value(c, fn, e->span); /* the constant pool takes one reference */
    plum_release(plum_obj(&fn->obj));
}

static void compile_expr(Compiler *c, const PlumExpr *e) {
    uint32_t line = e->span.line;
    switch (e->kind) {
    case PLUM_EXPR_INT:
        emit_const(c, plum_int(e->as.i), e->span);
        return;
    case PLUM_EXPR_FLOAT:
        emit_const(c, plum_float(e->as.f), e->span);
        return;
    case PLUM_EXPR_STRING:
        emit_const(c, plum_obj(&plum_string_new(e->as.str.chars, e->as.str.len)->obj), e->span);
        return;
    case PLUM_EXPR_INTERP:
        compile_interp(c, e);
        return;
    case PLUM_EXPR_BOOL:
        op(c, e->as.b ? PLUM_OP_TRUE : PLUM_OP_FALSE, line);
        return;
    case PLUM_EXPR_NONE:
        op(c, PLUM_OP_NONE, line);
        return;
    case PLUM_EXPR_NAME:
        load_symbol(c, e->symbol, e->span);
        return;
    case PLUM_EXPR_UNARY:
        compile_expr(c, e->as.unary.operand);
        switch (e->as.unary.op) {
        case PLUM_TOK_MINUS:
            op(c, ty_is(e->ty, PLUM_TY_FLOAT) ? PLUM_OP_NEG_FLOAT : PLUM_OP_NEG_INT, line);
            break;
        case PLUM_TOK_TILDE:
            op(c, PLUM_OP_BNOT, line);
            break;
        default:
            op(c, PLUM_OP_NOT, line);
            break;
        }
        return;
    case PLUM_EXPR_BINARY:
        compile_binary(c, e);
        return;
    case PLUM_EXPR_AS:
        compile_expr(c, e->as.cast.value);
        if (int_like(e->as.cast.value->ty) && ty_is(e->ty, PLUM_TY_FLOAT)) {
            op(c, PLUM_OP_INT_TO_FLOAT, line);
        } else if (ty_is(e->as.cast.value->ty, PLUM_TY_FLOAT) && int_like(e->ty)) {
            op2(c, PLUM_OP_CALL_BUILTIN, PLUM_BUILTIN_INT, 1, line);
        }
        return;
    case PLUM_EXPR_CALL:
        compile_call(c, e);
        return;
    case PLUM_EXPR_IF: {
        compile_expr(c, e->as.if_.cond);
        size_t to_else = emit_jump(c, PLUM_OP_JUMP_IF_FALSE, line);
        compile_as(c, e->as.if_.then, e->ty);
        size_t to_end = emit_jump(c, PLUM_OP_JUMP, line);
        patch_jump(c, to_else, e->span);
        compile_as(c, e->as.if_.otherwise, e->ty);
        patch_jump(c, to_end, e->span);
        return;
    }
    case PLUM_EXPR_FIELD:
        compile_field(c, e);
        return;
    case PLUM_EXPR_IS: {
        const PlumType *ty = e->as.cast.type;
        PlumSymbol *tree = ty->kind == PLUM_TYPE_NAME ? ty->as.name.symbol : NULL;
        if (tree == NULL || tree->kind != PLUM_SYM_TREE) {
            unsupported(c, e->span, "'is' with traits and other types", 22);
            return;
        }
        compile_expr(c, e->as.cast.value);
        op1(c, PLUM_OP_IS_TREE, tree_constant(c, tree, e->span), line);
        return;
    }
    case PLUM_EXPR_SELF:
    case PLUM_EXPR_SUPER:
        unsupported(c, e->span, "methods (self and super)", 22);
        return;
    case PLUM_EXPR_LIST:
        compile_list(c, e);
        return;
    case PLUM_EXPR_INDEX:
        compile_index(c, e);
        return;
    case PLUM_EXPR_MAP:
        compile_map(c, e);
        return;
    case PLUM_EXPR_RANGE:
        return; /* not a value: the checker allows it only where it is used */
    case PLUM_EXPR_LAMBDA:
        compile_lambda(c, e);
        return;
    case PLUM_EXPR_MATCH:
        unsupported(c, e->span, "match", 22);
        return;
    case PLUM_EXPR_TRY: {
        /* try expr: the value, or none if computing it throws */
        size_t to_none = emit_jump(c, PLUM_OP_TRY, line);
        compile_expr(c, e->as.inner);
        op1(c, PLUM_OP_END_TRY, 1, line);
        size_t to_end = emit_jump(c, PLUM_OP_JUMP, line);
        patch_jump(c, to_none, e->span);
        op(c, PLUM_OP_POP, line); /* the error */
        op(c, PLUM_OP_NONE, line);
        patch_jump(c, to_end, e->span);
        return;
    }
    case PLUM_EXPR_SPAWN:
        unsupported(c, e->span, "spawn", 27);
        return;
    case PLUM_EXPR_WILDCARD:
    case PLUM_EXPR_ERROR:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static void compile_block(Compiler *c, const PlumStmt *block) {
    int saved = c->fs->next_slot; /* slots of the block's variables are reused after it */
    if (block->kind == PLUM_STMT_BLOCK) {
        for (size_t i = 0; i < block->as.block.count; i++) {
            compile_stmt(c, block->as.block.stmts[i]);
        }
    } else {
        compile_stmt(c, block);
    }
    c->fs->next_slot = saved;
}

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

/*
 * Leaving tries early (return, break, continue): end each try's handlers,
 * then run its finally, innermost first, down to `until`. The handlers end
 * first so an error in a finally isn't caught by its own try.
 */
static void leave_tries(Compiler *c, Try *until, uint32_t line) {
    Try *saved = c->fs->tries;
    for (Try *t = saved; t != until && t != NULL; t = t->outer) {
        if (t->handlers > 0) {
            op1(c, PLUM_OP_END_TRY, (uint16_t)t->handlers, line);
        }
        if (t->finally != NULL) {
            c->fs->tries = t->outer; /* code in the finally is outside this try */
            compile_block(c, t->finally);
        }
    }
    c->fs->tries = saved;
}

/*
 * try { body } catch e: A { } catch e { } finally { }
 *
 *       TRY to_finally          (with a finally)
 *       TRY to_catches          (with catches)
 *       body
 *       END_TRY 1
 *       JUMP after
 *   to_catches:                 the error is on the stack
 *       DUP, IS_TREE A, JUMP_IF_FALSE next ; SET_LOCAL e ; catch body ; JUMP after
 *   next: ... a plain catch takes anything ...
 *       THROW                   no catch matched: on to the next try out
 *   after:
 *       END_TRY 1               (with a finally)
 *       finally
 *       JUMP end
 *   to_finally:                 an error got past the catches
 *       SET_LOCAL hidden ; finally ; GET_LOCAL hidden ; THROW
 *   end:
 */
static void compile_try(Compiler *c, const PlumStmt *s) {
    uint32_t line = s->span.line;
    const PlumStmt *finally = s->as.try_.finally;
    size_t catches = s->as.try_.catch_count;
    Try t = {c->fs->tries, 0, finally};
    c->fs->tries = &t;

    size_t to_finally = 0, to_catches = 0;
    if (finally != NULL) {
        to_finally = emit_jump(c, PLUM_OP_TRY, line);
        t.handlers++;
    }
    if (catches > 0) {
        to_catches = emit_jump(c, PLUM_OP_TRY, line);
        t.handlers++;
    }
    compile_block(c, s->as.try_.body);

    PLUM_ARRAY(size_t) after = {0};
    if (catches > 0) {
        op1(c, PLUM_OP_END_TRY, 1, line);
        t.handlers--; /* the catches run outside their own try */
        plum_array_push(&after, emit_jump(c, PLUM_OP_JUMP, line));
        patch_jump(c, to_catches, s->span);
        bool caught_all = false;
        for (size_t i = 0; i < catches && !caught_all; i++) {
            const PlumCatch *cat = &s->as.try_.catches[i];
            uint32_t at = cat->span.line;
            size_t next = 0;
            PlumSymbol *tree = cat->type != NULL && cat->type->kind == PLUM_TYPE_NAME
                                   ? cat->type->as.name.symbol
                                   : NULL;
            bool test =
                tree != NULL && !(is_builtin_error(tree) && strcmp(tree->name, "Error") == 0);
            if (test) {
                op(c, PLUM_OP_DUP, at);
                op1(c, PLUM_OP_IS_TREE, tree_constant(c, tree, cat->span), at);
                next = emit_jump(c, PLUM_OP_JUMP_IF_FALSE, at);
            } else {
                caught_all = true; /* the catches after this one can't be reached */
            }
            int saved = c->fs->next_slot;
            uint16_t slot = new_local(c, cat->symbol, cat->span);
            op1(c, PLUM_OP_SET_LOCAL, slot, at);
            box_if_captured(c, cat->symbol, cat->span);
            compile_block(c, cat->body);
            c->fs->next_slot = saved;
            plum_array_push(&after, emit_jump(c, PLUM_OP_JUMP, at));
            if (test) {
                patch_jump(c, next, cat->span);
            }
        }
        if (!caught_all) {
            op(c, PLUM_OP_THROW, line); /* not one of ours */
        }
    }
    for (size_t i = 0; i < after.len; i++) {
        patch_jump(c, after.items[i], s->span);
    }
    plum_array_free(&after);
    c->fs->tries = t.outer;

    if (finally != NULL) {
        op1(c, PLUM_OP_END_TRY, 1, line);
        compile_block(c, finally);
        size_t to_end = emit_jump(c, PLUM_OP_JUMP, line);
        patch_jump(c, to_finally, s->span);
        int saved = c->fs->next_slot;
        uint16_t hidden = new_local(c, NULL, s->span);
        op1(c, PLUM_OP_SET_LOCAL, hidden, line);
        compile_block(c, finally);
        op1(c, PLUM_OP_GET_LOCAL, hidden, line);
        op(c, PLUM_OP_THROW, line); /* on its way out, first place kept */
        c->fs->next_slot = saved;
        patch_jump(c, to_end, s->span);
    }
}

static void begin_loop(Compiler *c, Loop *loop, bool is_for) {
    memset(loop, 0, sizeof *loop);
    loop->outer = c->fs->loop;
    loop->is_for = is_for;
    loop->start = chunk(c)->code.len;
    loop->tries = c->fs->tries;
    c->fs->loop = loop;
}

static void patch_all(Compiler *c, size_t *at, size_t n, PlumSpan span) {
    for (size_t i = 0; i < n; i++) {
        patch_jump(c, at[i], span);
    }
}

static void end_loop(Compiler *c, Loop *loop, PlumSpan span) {
    patch_all(c, loop->breaks.items, loop->breaks.len, span);
    plum_array_free(&loop->breaks);
    plum_array_free(&loop->continues);
    c->fs->loop = loop->outer;
}

static void compile_while(Compiler *c, const PlumStmt *s) {
    Loop loop;
    begin_loop(c, &loop, false);
    compile_expr(c, s->as.while_.cond);
    size_t exit = emit_jump(c, PLUM_OP_JUMP_IF_FALSE, s->span.line);
    compile_block(c, s->as.while_.body);
    emit_loop(c, loop.start, s->span);
    patch_jump(c, exit, s->span);
    end_loop(c, &loop, s->span);
}

/*
 * for x in items / for i, x in items.enumerate(): the list (a snapshot,
 * since lists are values) and a position live in hidden slots; FOR_EACH
 * puts the next item in the loop variable or leaves the loop.
 */
static void compile_for_each(Compiler *c, const PlumStmt *s) {
    const PlumExpr *it = s->as.for_.iterable;
    bool enumerate = it->kind == PLUM_EXPR_CALL && it->as.call.callee->kind == PLUM_EXPR_FIELD &&
                     strcmp(it->as.call.callee->as.field.name, "enumerate") == 0;
    const PlumExpr *source = enumerate ? it->as.call.callee->as.field.object : it;
    if (!ty_is(source->ty, PLUM_TY_LIST) && !ty_is(source->ty, PLUM_TY_MAP) &&
        !ty_is(source->ty, PLUM_TY_SET) && !ty_is(source->ty, PLUM_TY_STR)) {
        unsupported(c, it->span, "loops over this", 20);
        return;
    }
    bool two = enumerate || s->as.for_.second != NULL; /* i, x or key, value */
    uint32_t line = s->span.line;
    int saved = c->fs->next_slot;
    uint16_t list = new_local(c, NULL, s->span);
    new_local(c, NULL, s->span); /* list + 1: the position */
    uint16_t var = new_local(c, s->symbol, s->span);
    if (two) {
        new_local(c, s->symbol2, s->span); /* var + 1: the item, or the value */
    }
    compile_expr(c, source);
    op1(c, PLUM_OP_SET_LOCAL, list, line);
    emit_const(c, plum_int(0), s->span);
    op1(c, PLUM_OP_SET_LOCAL, (uint16_t)(list + 1), line);

    Loop loop;
    begin_loop(c, &loop, false); /* continue goes back to FOR_EACH */
    op2(c, two ? PLUM_OP_FOR_EACH2 : PLUM_OP_FOR_EACH, list, var, line);
    plum_chunk_write_u16(chunk(c), 0xFFFF, line);
    size_t exit_at = chunk(c)->code.len - 2;
    box_if_captured(c, s->symbol, s->span); /* a fresh cell each time round */
    if (two) {
        box_if_captured(c, s->symbol2, s->span);
    }
    compile_block(c, s->as.for_.body);
    emit_loop(c, loop.start, s->span);
    patch_jump(c, exit_at, s->span);
    end_loop(c, &loop, s->span);
    c->fs->next_slot = saved;
}

/*
 * for i in a..b: i and a hidden end live in neighbouring slots.
 *
 *     i = a; end = b
 *   top: FOR_RANGE i  -> exit when i passes end
 *     body
 *   step: FOR_NEXT i  -> back to top (stops at an inclusive end first)
 *   exit:
 */
static void compile_for(Compiler *c, const PlumStmt *s) {
    const PlumExpr *it = s->as.for_.iterable;
    if (it->kind != PLUM_EXPR_RANGE) {
        compile_for_each(c, s);
        return;
    }
    uint32_t line = s->span.line;
    int saved = c->fs->next_slot;
    /* A captured loop variable gets a fresh cell each time round, so the
     * loop counts in a hidden slot and copies the count into it. */
    bool boxed = s->symbol != NULL && s->symbol->captured;
    uint16_t var = new_local(c, boxed ? NULL : s->symbol, s->span); /* `for _ in`: no symbol */
    new_local(c, NULL, s->span);                                    /* var + 1: the end */
    uint16_t user = boxed ? new_local(c, s->symbol, s->span) : var;
    bool inclusive = it->as.range.inclusive;

    compile_expr(c, it->as.range.low);
    op1(c, PLUM_OP_SET_LOCAL, var, line);
    compile_expr(c, it->as.range.high);
    op1(c, PLUM_OP_SET_LOCAL, (uint16_t)(var + 1), line);

    Loop loop;
    begin_loop(c, &loop, true);
    size_t top = chunk(c)->code.len;
    op2(c, PLUM_OP_FOR_RANGE, var, inclusive, line);
    plum_chunk_write_u16(chunk(c), 0xFFFF, line);
    size_t exit_at = chunk(c)->code.len - 2;
    if (boxed) {
        op1(c, PLUM_OP_GET_LOCAL, var, line);
        op1(c, PLUM_OP_SET_LOCAL, user, line);
        op1(c, PLUM_OP_BOX_LOCAL, user, line);
    }

    compile_block(c, s->as.for_.body);

    patch_all(c, loop.continues.items, loop.continues.len, s->span);
    op2(c, PLUM_OP_FOR_NEXT, var, inclusive, line);
    size_t back = chunk(c)->code.len + 2 - top;
    plum_chunk_write_u16(chunk(c), (uint16_t)(back <= 0xFFFF ? back : 0), line);
    if (back > 0xFFFF) {
        plum_diag_error(c->diags, s->span, "E0602",
                        "this loop is too long; split it into functions");
    }
    patch_jump(c, exit_at, s->span);
    end_loop(c, &loop, s->span);
    c->fs->next_slot = saved;
}

static PlumTokenKind compound_operator(PlumTokenKind tok);

/*
 * obj.f = v and obj.f op= v. Objects are shared, so SET_FIELD changes the
 * one object every holder sees. The object is computed once.
 */
static void compile_field_assign(Compiler *c, const PlumStmt *s) {
    const PlumExpr *target = s->as.assign.target;
    PlumSymbol *tree = tree_of(target->as.field.object->ty);
    int index = tree != NULL ? field_index(tree, target->as.field.name) : -1;
    if (index < 0 || target->as.field.optional) {
        unsupported(c, target->span, "assigning to this field", 22);
        return;
    }
    uint32_t line = s->span.line;
    int saved = c->fs->next_slot;
    uint16_t obj = new_local(c, NULL, s->span);
    compile_expr(c, target->as.field.object);
    op1(c, PLUM_OP_SET_LOCAL, obj, line);
    op1(c, PLUM_OP_GET_LOCAL, obj, line);
    if (s->as.assign.op == PLUM_TOK_EQ) {
        compile_as(c, s->as.assign.value, target->ty);
    } else {
        op1(c, PLUM_OP_GET_LOCAL, obj, line);
        op1(c, PLUM_OP_GET_FIELD, (uint16_t)index, line);
        arithmetic(c, compound_operator(s->as.assign.op), NULL, target->ty, s->as.assign.value,
                   line);
    }
    op1(c, PLUM_OP_SET_FIELD, (uint16_t)index, line);
    c->fs->next_slot = saved;
}

/*
 * grid[i][j] = v and grid[i][j] op= v: each key is computed once into a
 * hidden slot; SET_PATH takes the keys and the new value.
 */
static void compile_path_assign(Compiler *c, const PlumStmt *s) {
    const PlumExpr *target = s->as.assign.target;
    const PlumExpr *keys[MAX_PATH];
    int depth = 0;
    PlumSymbol *base = path_of(c, target, keys, &depth);
    if (base == NULL) {
        unsupported(c, target->span, "assigning into this", 22);
        return;
    }
    uint32_t line = s->span.line;
    int saved = c->fs->next_slot;
    uint16_t first = 0;
    for (int i = 0; i < depth; i++) {
        uint16_t slot = new_local(c, NULL, s->span);
        if (i == 0) {
            first = slot;
        }
        compile_key(c, keys[i]);
        op1(c, PLUM_OP_SET_LOCAL, slot, line);
    }
    for (int i = 0; i < depth; i++) {
        op1(c, PLUM_OP_GET_LOCAL, (uint16_t)(first + i), line);
    }
    if (s->as.assign.op == PLUM_TOK_EQ) {
        compile_as(c, s->as.assign.value, target->ty);
    } else {
        /* the old value: down the same keys */
        load_symbol(c, base, s->span);
        for (int i = 0; i < depth; i++) {
            op1(c, PLUM_OP_GET_LOCAL, (uint16_t)(first + i), line);
            op(c,
               ty_is(keys[i]->as.index.object->ty, PLUM_TY_MAP) ? PLUM_OP_INDEX_MAP
                                                                : PLUM_OP_INDEX_LIST,
               line);
        }
        arithmetic(c, compound_operator(s->as.assign.op), NULL, target->ty, s->as.assign.value,
                   line);
    }
    uint16_t n;
    PlumOpcode o = place_op(c, base, PLUM_OP_SET_PATH_LOCAL, PLUM_OP_SET_PATH_GLOBAL,
                            PLUM_OP_SET_PATH_CAPTURE, &n);
    op2(c, o, n, (uint16_t)depth, line);
    c->fs->next_slot = saved;
}

/*
 * xs[i] = v and xs[i] op= v on a list variable. The index is computed once
 * into a hidden slot; SET_INDEX takes the index and the new value.
 */
static void compile_index_assign(Compiler *c, const PlumStmt *s) {
    const PlumExpr *target = s->as.assign.target;
    const PlumExpr *obj = target->as.index.object;
    PlumSymbol *place = place_of(c, obj);
    bool is_map = ty_is(obj->ty, PLUM_TY_MAP);
    if (place == NULL && target->as.index.index->kind != PLUM_EXPR_RANGE) {
        compile_path_assign(c, s);
        return;
    }
    if ((!ty_is(obj->ty, PLUM_TY_LIST) && !is_map) || place == NULL ||
        target->as.index.index->kind == PLUM_EXPR_RANGE) {
        unsupported(c, target->span, "assigning to a slice", 20);
        return;
    }
    uint32_t line = s->span.line;
    int saved = c->fs->next_slot;
    uint16_t idx = new_local(c, NULL, s->span);
    if (is_map) {
        compile_as(c, target->as.index.index, obj->ty->as.map.key);
    } else {
        compile_expr(c, target->as.index.index);
    }
    op1(c, PLUM_OP_SET_LOCAL, idx, line);
    op1(c, PLUM_OP_GET_LOCAL, idx, line);
    if (s->as.assign.op == PLUM_TOK_EQ) {
        compile_as(c, s->as.assign.value, target->ty);
    } else {
        load_symbol(c, place, obj->span);
        op1(c, PLUM_OP_GET_LOCAL, idx, line);
        op(c, is_map ? PLUM_OP_INDEX_MAP : PLUM_OP_INDEX_LIST, line);
        arithmetic(c, compound_operator(s->as.assign.op), NULL, target->ty, s->as.assign.value,
                   line);
    }
    uint16_t n;
    PlumOpcode o = place_op(c, place, PLUM_OP_SET_INDEX_LOCAL, PLUM_OP_SET_INDEX_GLOBAL,
                            PLUM_OP_SET_INDEX_CAPTURE, &n);
    op1(c, o, n, line);
    c->fs->next_slot = saved;
}

static PlumTokenKind compound_operator(PlumTokenKind tok) {
    static const struct {
        PlumTokenKind assign, bin;
    } ops[] = {
        {PLUM_TOK_PLUS_EQ, PLUM_TOK_PLUS},
        {PLUM_TOK_MINUS_EQ, PLUM_TOK_MINUS},
        {PLUM_TOK_STAR_EQ, PLUM_TOK_STAR},
        {PLUM_TOK_SLASH_EQ, PLUM_TOK_SLASH},
        {PLUM_TOK_SLASH_SLASH_EQ, PLUM_TOK_SLASH_SLASH},
        {PLUM_TOK_PERCENT_EQ, PLUM_TOK_PERCENT},
        {PLUM_TOK_STAR_STAR_EQ, PLUM_TOK_STAR_STAR},
        {PLUM_TOK_AMP_EQ, PLUM_TOK_AMP},
        {PLUM_TOK_PIPE_EQ, PLUM_TOK_PIPE},
        {PLUM_TOK_CARET_EQ, PLUM_TOK_CARET},
        {PLUM_TOK_LESS_LESS_EQ, PLUM_TOK_LESS_LESS},
        {PLUM_TOK_GREATER_GREATER_EQ, PLUM_TOK_GREATER_GREATER},
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        if (ops[i].assign == tok) {
            return ops[i].bin;
        }
    }
    return PLUM_TOK_PLUS;
}

static void compile_assign(Compiler *c, const PlumStmt *s) {
    const PlumExpr *target = s->as.assign.target;
    if (target->kind == PLUM_EXPR_INDEX) {
        compile_index_assign(c, s);
        return;
    }
    if (target->kind == PLUM_EXPR_FIELD) {
        compile_field_assign(c, s);
        return;
    }
    if (target->kind != PLUM_EXPR_NAME) {
        unsupported(c, target->span, "assigning to this", 22);
        return;
    }
    PlumTokenKind tok = s->as.assign.op;
    if (tok == PLUM_TOK_EQ) {
        compile_as(c, s->as.assign.value, target->ty);
    } else {
        PlumTokenKind bin = compound_operator(tok);
        load_symbol(c, target->symbol, target->span);
        arithmetic(c, bin, NULL, target->ty, s->as.assign.value, s->span.line);
    }
    store_symbol(c, target->symbol, target->span);
}

static void compile_function(Compiler *c, PlumFunction *fn, const PlumBranch *b, int depth,
                             const PlumTy *result, PlumSymbol *self);

static void compile_stmt(Compiler *c, const PlumStmt *s) {
    uint32_t line = s->span.line;
    switch (s->kind) {
    case PLUM_STMT_EXPR:
        compile_expr(c, s->as.expr);
        op(c, PLUM_OP_POP, line);
        return;
    case PLUM_STMT_DECL: {
        PlumSymbol *sym = s->symbol;
        compile_as(c, s->as.decl.value, sym != NULL ? sym->ty : NULL);
        if (sym != NULL && !sym->global) {
            uint16_t slot = new_local(c, sym, s->span); /* after the value: plant x = x + 1 */
            op1(c, PLUM_OP_SET_LOCAL, slot, line);
            box_if_captured(c, sym, s->span);
        } else if (sym != NULL) {
            store_symbol(c, sym, s->span);
        }
        return;
    }
    case PLUM_STMT_ASSIGN:
        compile_assign(c, s);
        return;
    case PLUM_STMT_BLOCK:
        compile_block(c, s);
        return;
    case PLUM_STMT_IF:
    case PLUM_STMT_IF_PLANT: {
        int saved = c->fs->next_slot;
        size_t to_else;
        if (s->kind == PLUM_STMT_IF) {
            compile_expr(c, s->as.if_.cond);
            to_else = emit_jump(c, PLUM_OP_JUMP_IF_FALSE, line);
        } else {
            /* if plant x = value: x holds the value; skip when it is none */
            compile_expr(c, s->as.if_.value);
            uint16_t slot = new_local(c, s->symbol, s->span);
            op1(c, PLUM_OP_SET_LOCAL, slot, line);
            op1(c, PLUM_OP_GET_LOCAL, slot, line);
            op(c, PLUM_OP_IS_NONE, line);
            op(c, PLUM_OP_NOT, line);
            to_else = emit_jump(c, PLUM_OP_JUMP_IF_FALSE, line);
            box_if_captured(c, s->symbol, s->span);
        }
        compile_block(c, s->as.if_.then);
        c->fs->next_slot = saved;
        if (s->as.if_.otherwise != NULL) {
            size_t to_end = emit_jump(c, PLUM_OP_JUMP, line);
            patch_jump(c, to_else, s->span);
            compile_block(c, s->as.if_.otherwise);
            patch_jump(c, to_end, s->span);
        } else {
            patch_jump(c, to_else, s->span);
        }
        return;
    }
    case PLUM_STMT_WHILE:
        compile_while(c, s);
        return;
    case PLUM_STMT_FOR:
        compile_for(c, s);
        return;
    case PLUM_STMT_BREAK:
    case PLUM_STMT_CONTINUE: {
        Loop *loop = c->fs->loop;
        if (loop == NULL) {
            return; /* reported by the flow checks */
        }
        leave_tries(c, loop->tries, line);
        if (s->kind == PLUM_STMT_CONTINUE && !loop->is_for) {
            emit_loop(c, loop->start, s->span);
            return;
        }
        size_t at = emit_jump(c, PLUM_OP_JUMP, line);
        if (s->kind == PLUM_STMT_BREAK) {
            plum_array_push(&loop->breaks, at);
        } else {
            plum_array_push(&loop->continues, at);
        }
        return;
    }
    case PLUM_STMT_RETURN:
        if (s->as.expr == NULL) {
            leave_tries(c, NULL, line);
            op(c, PLUM_OP_RETURN_NONE, line);
        } else {
            compile_as(c, s->as.expr, c->result);
            leave_tries(c, NULL, line); /* the value waits on the stack */
            op(c, PLUM_OP_RETURN, line);
        }
        return;
    case PLUM_STMT_BRANCH: {
        /* A function inside a function: a local holding a function or
         * closure. If it calls itself, its slot is a cell made first, so
         * the closure can capture it. */
        PlumFunction *fn = plum_function_new(s->as.branch.name, (int)s->as.branch.param_count);
        uint16_t slot = new_local(c, s->symbol, s->span);
        bool boxed = s->symbol != NULL && s->symbol->captured;
        if (boxed) {
            op(c, PLUM_OP_NONE, line);
            op1(c, PLUM_OP_SET_LOCAL, slot, line);
            op1(c, PLUM_OP_BOX_LOCAL, slot, line);
        }
        plum_retain(plum_obj(&fn->obj));
        compile_function(c, fn, &s->as.branch, c->fs->depth + 1,
                         s->symbol != NULL && s->symbol->ty != NULL ? s->symbol->ty->as.func.result
                                                                    : NULL,
                         s->symbol);
        emit_function_value(c, fn, s->span);
        plum_release(plum_obj(&fn->obj));
        op1(c, boxed ? PLUM_OP_SET_CELL : PLUM_OP_SET_LOCAL, slot, line);
        return;
    }
    case PLUM_STMT_THROW:
        compile_expr(c, s->as.expr);
        op(c, PLUM_OP_THROW, line);
        return;
    case PLUM_STMT_TRY:
        compile_try(c, s);
        return;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
    case PLUM_STMT_KIND:
    case PLUM_STMT_SEED:
    case PLUM_STMT_ERROR:
        return; /* declarations: nothing runs */
    }
}

static void compile_function_body(Compiler *c, PlumFunction *fn, const PlumParam *params,
                                  size_t param_count, const PlumExpr *arrow, const PlumStmt *body,
                                  int depth, const PlumTy *result, PlumSymbol *self) {
    FnState fs;
    memset(&fs, 0, sizeof fs);
    fs.enclosing = c->fs;
    fs.fn = fn;
    fs.depth = depth;
    fs.self = self;
    PlumTy *outer_result = c->result;
    c->fs = &fs;
    c->result = (PlumTy *)result;

    for (size_t i = 0; i < param_count; i++) {
        new_local(c, params[i].symbol, params[i].span);
    }
    for (size_t i = 0; i < param_count; i++) {
        box_if_captured(c, params[i].symbol, params[i].span); /* shared with closures */
    }
    bool returns_value = result != NULL && result->kind != PLUM_TY_NONE;
    if (arrow != NULL) {
        compile_as(c, arrow, result);
        op(c, returns_value ? PLUM_OP_RETURN : PLUM_OP_POP, arrow->span.line);
        if (!returns_value) {
            op(c, PLUM_OP_RETURN_NONE, arrow->span.line);
        }
    } else if (body != NULL) {
        const PlumStmt *const *stmts = (const PlumStmt *const *)body->as.block.stmts;
        size_t n = body->kind == PLUM_STMT_BLOCK ? body->as.block.count : 0;
        for (size_t i = 0; i < n; i++) {
            compile_stmt(c, stmts[i]);
        }
        op(c, PLUM_OP_RETURN_NONE, body->span.line);
    }

    /* What a closure of this function captures, in order. */
    fn->capture_count = (int)fs.captures.len;
    if (fs.captures.len > 0) {
        fn->captures = plum_alloc_zeroed(fs.captures.len, sizeof(PlumCapture));
        for (size_t i = 0; i < fs.captures.len; i++) {
            fn->captures[i] = fs.captures.items[i].from;
        }
    }
    plum_array_free(&fs.captures);
    c->fs = fs.enclosing;
    c->result = outer_result;
}

static void compile_function(Compiler *c, PlumFunction *fn, const PlumBranch *b, int depth,
                             const PlumTy *result, PlumSymbol *self) {
    compile_function_body(c, fn, b->params, b->param_count, b->arrow, b->body, depth, result, self);
}

PlumCompiled plum_compile(const PlumProgram *program, PlumInterner *names, PlumDiagList *diags) {
    Compiler c;
    memset(&c, 0, sizeof c);
    c.diags = diags;
    c.names = names;
    plum_map_init(&c.trees);
    size_t errors_before = diags->errors;

    PlumFunction *main = plum_function_new("<main>", 0);
    FnState fs;
    memset(&fs, 0, sizeof fs);
    fs.fn = main;
    c.fs = &fs;

    /* Every module variable and function gets a global slot. */
    for (size_t i = 0; i < program->count; i++) {
        PlumSymbol *sym = program->stmts[i]->symbol;
        PlumStmtKind k = program->stmts[i]->kind;
        if (sym != NULL && (k == PLUM_STMT_DECL || k == PLUM_STMT_BRANCH)) {
            sym->slot = c.global_count++;
            sym->has_slot = true;
        }
    }

    /* Module functions exist before the first statement runs, so they can
     * be called before their declaration. */
    for (size_t i = 0; i < program->count; i++) {
        const PlumStmt *s = program->stmts[i];
        if (s->kind != PLUM_STMT_BRANCH || s->symbol == NULL) {
            continue;
        }
        PlumFunction *fn = plum_function_new(s->as.branch.name, (int)s->as.branch.param_count);
        plum_retain(plum_obj(&fn->obj));
        emit_const(&c, plum_obj(&fn->obj), s->span);
        op1(&c, PLUM_OP_SET_GLOBAL, (uint16_t)s->symbol->slot, s->span.line);
        compile_function(&c, fn, &s->as.branch, 1,
                         s->symbol->ty != NULL ? s->symbol->ty->as.func.result : NULL, NULL);
        plum_release(plum_obj(&fn->obj));
    }

    for (size_t i = 0; i < program->count; i++) {
        const PlumStmt *s = program->stmts[i];
        if (s->kind != PLUM_STMT_BRANCH) {
            compile_stmt(&c, s);
        }
    }
    op(&c, PLUM_OP_RETURN_NONE,
       program->count > 0 ? program->stmts[program->count - 1]->span.line : 1);

    PlumCompiled out = {main, c.global_count, {NULL}};
    for (int k = 0; k < PLUM_ERROR_COUNT; k++) {
        out.errors[k] = builtin_error_info(&c, plum_error_name((PlumErrorKind)k));
        plum_retain(plum_obj(&out.errors[k]->obj));
    }
    /* the constants that use a tree info hold it now */
    size_t cursor = 0;
    const PlumMapEntry *entry;
    while ((entry = plum_map_next(&c.trees, &cursor)) != NULL) {
        plum_release(plum_obj(&((PlumTreeInfo *)entry->value)->obj));
    }
    plum_map_free(&c.trees);
    if (diags->errors != errors_before) {
        plum_release(plum_obj(&main->obj));
        out.main = NULL;
    }
    return out;
}

void plum_compiled_free(PlumCompiled *compiled) {
    if (compiled->main != NULL) {
        plum_release(plum_obj(&compiled->main->obj));
        compiled->main = NULL;
    }
    for (int k = 0; k < PLUM_ERROR_COUNT; k++) {
        if (compiled->errors[k] != NULL) {
            plum_release(plum_obj(&compiled->errors[k]->obj));
            compiled->errors[k] = NULL;
        }
    }
}
