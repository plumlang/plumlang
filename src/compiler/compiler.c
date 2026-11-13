/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "compiler/compiler.h"

#include "check/resolve.h"
#include "check/types.h"
#include "compiler/chunk.h"
#include "util/alloc.h"
#include "util/array.h"
#include "vm/methods.h"

#include <string.h>

#define NO_FORMAT 0xFFFF
#define MAX_SLOTS 0xFFFF

/* An enclosing loop: where continue goes, and jumps to patch at its end. */
typedef struct Loop {
    struct Loop *outer;
    bool is_for;  /* continue jumps forward to the step (for) or back (while) */
    size_t start; /* while: where continue loops back to */
    PLUM_ARRAY(size_t) breaks;
    PLUM_ARRAY(size_t) continues; /* for loops: patched to the step */
} Loop;

/* The function being compiled. */
typedef struct FnState {
    struct FnState *enclosing;
    PlumFunction *fn;
    int depth;     /* the resolver's function depth of its body */
    int next_slot; /* the next free local slot */
    Loop *loop;
} FnState;

typedef struct Compiler {
    PlumDiagList *diags;
    PlumInterner *names;
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

static void load_symbol(Compiler *c, PlumSymbol *sym, PlumSpan span) {
    if (!sym->has_slot) {
        unsupported(c, span, "this kind of name", 22);
        return;
    }
    if (sym->global) {
        op1(c, PLUM_OP_GET_GLOBAL, (uint16_t)sym->slot, span.line);
        return;
    }
    if (sym->function_depth != c->fs->depth) {
        unsupported(c, span, "closures (using a variable of an outer function)", 19);
        return;
    }
    op1(c, PLUM_OP_GET_LOCAL, (uint16_t)sym->slot, span.line);
}

static void store_symbol(Compiler *c, PlumSymbol *sym, PlumSpan span) {
    if (sym->global) {
        op1(c, PLUM_OP_SET_GLOBAL, (uint16_t)sym->slot, span.line);
    } else if (sym->function_depth != c->fs->depth) {
        unsupported(c, span, "closures (changing a variable of an outer function)", 19);
    } else {
        op1(c, PLUM_OP_SET_LOCAL, (uint16_t)sym->slot, span.line);
    }
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
        if (!ty_is(r->ty, PLUM_TY_LIST)) {
            unsupported(c, e->span, "'in' on maps, sets and strings", 20);
            return;
        }
        compile_as(c, l, r->ty->as.elem);
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

static void compile_index(Compiler *c, const PlumExpr *e) {
    const PlumExpr *obj = e->as.index.object, *idx = e->as.index.index;
    if (!ty_is(obj->ty, PLUM_TY_LIST)) {
        unsupported(c, e->span, "indexing maps and strings", 20);
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
    op(c, PLUM_OP_INDEX_LIST, e->span.line);
}

/* A variable a method can change in place, or NULL. */
static PlumSymbol *place_of(Compiler *c, const PlumExpr *obj) {
    if (obj->kind != PLUM_EXPR_NAME || obj->symbol == NULL || !obj->symbol->has_slot) {
        return NULL;
    }
    PlumSymbol *sym = obj->symbol;
    if (!sym->global && sym->function_depth != c->fs->depth) {
        return NULL; /* a closure's variable */
    }
    return sym;
}

/* xs.method(args): in place on a variable, or on a temporary value. */
static void compile_method(Compiler *c, const PlumExpr *e) {
    const PlumExpr *callee = e->as.call.callee;
    const PlumExpr *obj = callee->as.field.object;
    const PlumTy *fty = callee->ty;
    int m = plum_method_find(PLUM_RECEIVER_LIST, callee->as.field.name);
    if (m < 0 || !ty_is(fty, PLUM_TY_FUNC) || callee->as.field.optional) {
        unsupported(c, e->span, "this method", 24);
        return;
    }
    uint32_t line = e->span.line;
    uint16_t argc = (uint16_t)fty->as.func.param_count;
    PlumSymbol *place = plum_method_mutates((PlumMethod)m) ? place_of(c, obj) : NULL;
    if (place == NULL) {
        compile_expr(c, obj);
    }
    compile_args(c, e, fty, NULL);
    if (place == NULL) {
        op2(c, PLUM_OP_METHOD, (uint16_t)m, argc, line);
        return;
    }
    op2(c, place->global ? PLUM_OP_METHOD_GLOBAL : PLUM_OP_METHOD_LOCAL, (uint16_t)place->slot,
        (uint16_t)m, line);
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
    if (sym != NULL && (sym->kind == PLUM_SYM_TREE || sym->kind == PLUM_SYM_KIND)) {
        unsupported(c, e->span, "creating tree objects", 22);
        return;
    }
    if (callee->kind == PLUM_EXPR_FIELD) {
        if (ty_is(callee->as.field.object->ty, PLUM_TY_LIST)) {
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
    case PLUM_EXPR_SELF:
    case PLUM_EXPR_SUPER:
    case PLUM_EXPR_FIELD:
    case PLUM_EXPR_IS:
        unsupported(c, e->span, "trees, fields and methods", 22);
        return;
    case PLUM_EXPR_LIST:
        compile_list(c, e);
        return;
    case PLUM_EXPR_INDEX:
        compile_index(c, e);
        return;
    case PLUM_EXPR_MAP:
    case PLUM_EXPR_RANGE:
        unsupported(c, e->span, "maps and ranges as values", 20);
        return;
    case PLUM_EXPR_LAMBDA:
        unsupported(c, e->span, "lambdas", 19);
        return;
    case PLUM_EXPR_MATCH:
        unsupported(c, e->span, "match", 22);
        return;
    case PLUM_EXPR_TRY:
        unsupported(c, e->span, "try", 21);
        return;
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

static void begin_loop(Compiler *c, Loop *loop, bool is_for) {
    memset(loop, 0, sizeof *loop);
    loop->outer = c->fs->loop;
    loop->is_for = is_for;
    loop->start = chunk(c)->code.len;
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
    if (!ty_is(source->ty, PLUM_TY_LIST)) {
        unsupported(c, it->span, "loops over strings, maps and sets", 20);
        return;
    }
    uint32_t line = s->span.line;
    int saved = c->fs->next_slot;
    uint16_t list = new_local(c, NULL, s->span);
    new_local(c, NULL, s->span); /* list + 1: the position */
    uint16_t var = new_local(c, s->symbol, s->span);
    if (enumerate) {
        new_local(c, s->symbol2, s->span); /* var + 1: the item */
    }
    compile_expr(c, source);
    op1(c, PLUM_OP_SET_LOCAL, list, line);
    emit_const(c, plum_int(0), s->span);
    op1(c, PLUM_OP_SET_LOCAL, (uint16_t)(list + 1), line);

    Loop loop;
    begin_loop(c, &loop, false); /* continue goes back to FOR_EACH */
    op2(c, enumerate ? PLUM_OP_FOR_EACH2 : PLUM_OP_FOR_EACH, list, var, line);
    plum_chunk_write_u16(chunk(c), 0xFFFF, line);
    size_t exit_at = chunk(c)->code.len - 2;
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
    uint16_t var = new_local(c, s->symbol, s->span); /* `for _ in` has no symbol */
    new_local(c, NULL, s->span);                     /* var + 1: the end */
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
 * xs[i] = v and xs[i] op= v on a list variable. The index is computed once
 * into a hidden slot; SET_INDEX takes the index and the new value.
 */
static void compile_index_assign(Compiler *c, const PlumStmt *s) {
    const PlumExpr *target = s->as.assign.target;
    const PlumExpr *obj = target->as.index.object;
    PlumSymbol *place = place_of(c, obj);
    if (!ty_is(obj->ty, PLUM_TY_LIST) || place == NULL ||
        target->as.index.index->kind == PLUM_EXPR_RANGE) {
        unsupported(c, target->span, "assigning into maps, nested lists and slices", 20);
        return;
    }
    uint32_t line = s->span.line;
    int saved = c->fs->next_slot;
    uint16_t idx = new_local(c, NULL, s->span);
    compile_expr(c, target->as.index.index);
    op1(c, PLUM_OP_SET_LOCAL, idx, line);
    op1(c, PLUM_OP_GET_LOCAL, idx, line);
    if (s->as.assign.op == PLUM_TOK_EQ) {
        compile_as(c, s->as.assign.value, target->ty);
    } else {
        load_symbol(c, place, obj->span);
        op1(c, PLUM_OP_GET_LOCAL, idx, line);
        op(c, PLUM_OP_INDEX_LIST, line);
        arithmetic(c, compound_operator(s->as.assign.op), NULL, target->ty, s->as.assign.value,
                   line);
    }
    op1(c, place->global ? PLUM_OP_SET_INDEX_GLOBAL : PLUM_OP_SET_INDEX_LOCAL,
        (uint16_t)place->slot, line);
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
    if (target->kind != PLUM_EXPR_NAME) {
        unsupported(c, target->span, "assigning to fields", 22);
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
                             const PlumTy *result);

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
            new_local(c, sym, s->span); /* after the value: plant x = x + 1 */
        }
        if (sym != NULL) {
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
            op(c, PLUM_OP_RETURN_NONE, line);
        } else {
            compile_as(c, s->as.expr, c->result);
            op(c, PLUM_OP_RETURN, line);
        }
        return;
    case PLUM_STMT_BRANCH: {
        /* A function inside a function: a local holding a function value. */
        PlumFunction *fn = plum_function_new(s->as.branch.name, (int)s->as.branch.param_count);
        uint16_t slot = new_local(c, s->symbol, s->span);
        emit_const(c, plum_obj(&fn->obj), s->span);
        op1(c, PLUM_OP_SET_LOCAL, slot, line);
        compile_function(c, fn, &s->as.branch, c->fs->depth + 1,
                         s->symbol != NULL && s->symbol->ty != NULL ? s->symbol->ty->as.func.result
                                                                    : NULL);
        return;
    }
    case PLUM_STMT_THROW:
    case PLUM_STMT_TRY:
        unsupported(c, s->span, "throw and try", 21);
        return;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
    case PLUM_STMT_KIND:
    case PLUM_STMT_SEED:
    case PLUM_STMT_ERROR:
        return; /* declarations: nothing runs */
    }
}

static void compile_function(Compiler *c, PlumFunction *fn, const PlumBranch *b, int depth,
                             const PlumTy *result) {
    FnState fs;
    memset(&fs, 0, sizeof fs);
    fs.enclosing = c->fs;
    fs.fn = fn;
    fs.depth = depth;
    PlumTy *outer_result = c->result;
    c->fs = &fs;
    c->result = (PlumTy *)result;

    for (size_t i = 0; i < b->param_count; i++) {
        new_local(c, b->params[i].symbol, b->params[i].span);
    }
    if (b->arrow != NULL) {
        compile_as(c, b->arrow, result);
        op(c, result != NULL && result->kind != PLUM_TY_NONE ? PLUM_OP_RETURN : PLUM_OP_POP,
           b->arrow->span.line);
        if (result == NULL || result->kind == PLUM_TY_NONE) {
            op(c, PLUM_OP_RETURN_NONE, b->arrow->span.line);
        }
    } else if (b->body != NULL) {
        for (size_t i = 0; i < b->body->as.block.count; i++) {
            compile_stmt(c, b->body->as.block.stmts[i]);
        }
        op(c, PLUM_OP_RETURN_NONE, b->body->span.line);
    }
    c->fs = fs.enclosing;
    c->result = outer_result;
}

PlumCompiled plum_compile(const PlumProgram *program, PlumInterner *names, PlumDiagList *diags) {
    Compiler c;
    memset(&c, 0, sizeof c);
    c.diags = diags;
    c.names = names;
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
                         s->symbol->ty != NULL ? s->symbol->ty->as.func.result : NULL);
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

    PlumCompiled out = {main, c.global_count};
    if (diags->errors != errors_before) {
        plum_release(plum_obj(&main->obj));
        out.main = NULL;
    }
    return out;
}
