/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check/flow.h"

#include "check/resolve.h"
#include "check/types.h"

#include <stdio.h>
#include <string.h>

typedef struct Flow {
    PlumDiagList *diags;
    int loop_depth; /* loops around the current statement, in this function */
} Flow;

static bool match_exhaustive(Flow *f, const PlumExpr *e, bool report);

/* ------------------------------------------------------------------ */
/* Does a statement end the path?                                      */
/* ------------------------------------------------------------------ */

static bool is_exit_call(const PlumExpr *e) {
    return e->kind == PLUM_EXPR_CALL && e->as.call.callee->kind == PLUM_EXPR_NAME &&
           e->as.call.callee->symbol != NULL &&
           e->as.call.callee->symbol->kind == PLUM_SYM_BUILTIN_FUNC &&
           strcmp(e->as.call.callee->symbol->name, "exit") == 0;
}

/* Is there a break for this loop in `s` (not in loops or functions inside)? */
static bool has_break(const PlumStmt *s) {
    if (s == NULL) {
        return false;
    }
    switch (s->kind) {
    case PLUM_STMT_BREAK:
        return true;
    case PLUM_STMT_BLOCK:
        for (size_t i = 0; i < s->as.block.count; i++) {
            if (has_break(s->as.block.stmts[i])) {
                return true;
            }
        }
        return false;
    case PLUM_STMT_IF:
    case PLUM_STMT_IF_PLANT:
        return has_break(s->as.if_.then) || has_break(s->as.if_.otherwise);
    case PLUM_STMT_TRY:
        if (has_break(s->as.try_.body) || has_break(s->as.try_.finally)) {
            return true;
        }
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            if (has_break(s->as.try_.catches[i].body)) {
                return true;
            }
        }
        return false;
    case PLUM_STMT_EXPR:
        if (s->as.expr->kind == PLUM_EXPR_MATCH) {
            for (size_t i = 0; i < s->as.expr->as.match.arm_count; i++) {
                if (has_break(s->as.expr->as.match.arms[i].body)) {
                    return true;
                }
            }
        }
        return false;
    default:
        return false; /* nested loops have their own breaks */
    }
}

/*
 * True if control never reaches the statement after `s`: it returns,
 * throws or exits on every path, or (with `loops`) breaks or continues.
 */
static bool ends(Flow *f, const PlumStmt *s, bool loops) {
    if (s == NULL) {
        return false;
    }
    switch (s->kind) {
    case PLUM_STMT_RETURN:
    case PLUM_STMT_THROW:
        return true;
    case PLUM_STMT_BREAK:
    case PLUM_STMT_CONTINUE:
        return loops;
    case PLUM_STMT_EXPR: {
        const PlumExpr *e = s->as.expr;
        if (is_exit_call(e)) {
            return true;
        }
        if (e->kind == PLUM_EXPR_MATCH && e->as.match.arm_count > 0 &&
            match_exhaustive(f, e, false)) {
            for (size_t i = 0; i < e->as.match.arm_count; i++) {
                if (!ends(f, e->as.match.arms[i].body, loops)) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }
    case PLUM_STMT_BLOCK:
        for (size_t i = 0; i < s->as.block.count; i++) {
            if (ends(f, s->as.block.stmts[i], loops)) {
                return true;
            }
        }
        return false;
    case PLUM_STMT_IF:
    case PLUM_STMT_IF_PLANT:
        return s->as.if_.otherwise != NULL && ends(f, s->as.if_.then, loops) &&
               ends(f, s->as.if_.otherwise, loops);
    case PLUM_STMT_WHILE: {
        /* `while good` without a break only leaves by returning */
        const PlumExpr *cond = s->as.while_.cond;
        return cond->kind == PLUM_EXPR_BOOL && cond->as.b && !has_break(s->as.while_.body);
    }
    case PLUM_STMT_TRY: {
        if (ends(f, s->as.try_.finally, loops)) {
            return true;
        }
        if (!ends(f, s->as.try_.body, loops)) {
            return false;
        }
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            if (!ends(f, s->as.try_.catches[i].body, loops)) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* match covers every case                                             */
/* ------------------------------------------------------------------ */

static const char *case_name_of(const PlumExpr *p) {
    if (p->kind == PLUM_EXPR_NAME && p->symbol != NULL && p->symbol->kind == PLUM_SYM_CASE) {
        return p->symbol->name;
    }
    if (p->kind == PLUM_EXPR_CALL && p->as.call.callee->symbol != NULL &&
        p->as.call.callee->symbol->kind == PLUM_SYM_CASE) {
        return p->as.call.callee->symbol->name;
    }
    if (p->kind == PLUM_EXPR_FIELD) {
        return p->as.field.name; /* Kind.Case */
    }
    return NULL;
}

static bool match_exhaustive(Flow *f, const PlumExpr *e, bool report) {
    const PlumTy *subject = e->as.match.subject->ty;
    for (size_t i = 0; i < e->as.match.arm_count; i++) {
        const PlumMatchArm *arm = &e->as.match.arms[i];
        if (arm->pattern_count == 0) {
            return true; /* else */
        }
        for (size_t k = 0; k < arm->pattern_count; k++) {
            if (arm->patterns[k]->kind == PLUM_EXPR_WILDCARD) {
                return true;
            }
        }
    }
    if (subject == NULL || plum_ty_is_loose(subject)) {
        return true; /* already reported, or not known yet */
    }

    if (subject->kind == PLUM_TY_KIND && subject->as.decl->decl != NULL) {
        const PlumStmt *kind = subject->as.decl->decl;
        char missing[256] = "";
        size_t n_missing = 0;
        for (size_t c = 0; c < kind->as.kind.case_count; c++) {
            const char *name = kind->as.kind.cases[c].name;
            bool covered = false;
            for (size_t i = 0; i < e->as.match.arm_count && !covered; i++) {
                const PlumMatchArm *arm = &e->as.match.arms[i];
                for (size_t k = 0; k < arm->pattern_count; k++) {
                    const char *pn = case_name_of(arm->patterns[k]);
                    if (pn != NULL && strcmp(pn, name) == 0) {
                        covered = true;
                    }
                }
            }
            if (!covered) {
                if (n_missing < 4) {
                    size_t len = strlen(missing);
                    snprintf(missing + len, sizeof missing - len, "%s'%s'",
                             n_missing > 0 ? ", " : "", name);
                }
                n_missing++;
            }
        }
        if (n_missing == 0) {
            return true;
        }
        if (report) {
            plum_diag_error(f->diags, e->span, "E0503",
                            "this match doesn't cover %s %s%s of %s; add %s or an 'else' arm",
                            n_missing == 1 ? "the case" : "the cases", missing,
                            n_missing > 4 ? " and more" : "", subject->as.decl->name,
                            n_missing == 1 ? "it" : "them");
        }
        return false;
    }

    if (subject->kind == PLUM_TY_BOOL) {
        bool seen_good = false, seen_bad = false;
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            const PlumMatchArm *arm = &e->as.match.arms[i];
            for (size_t k = 0; k < arm->pattern_count; k++) {
                const PlumExpr *p = arm->patterns[k];
                if (p->kind == PLUM_EXPR_BOOL) {
                    seen_good |= p->as.b;
                    seen_bad |= !p->as.b;
                }
            }
        }
        if (seen_good && seen_bad) {
            return true;
        }
        if (report) {
            plum_diag_error(f->diags, e->span, "E0503",
                            "this match on a bool doesn't cover '%s'; add it or an 'else' arm",
                            seen_good ? "bad" : "good");
        }
        return false;
    }

    if (report) {
        char t[64];
        plum_diag_error(f->diags, e->span, "E0503",
                        "a match on %s can't list every value; add an 'else' arm for the rest",
                        plum_ty_text(subject, t, sizeof t));
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* The walk                                                            */
/* ------------------------------------------------------------------ */

static void walk_stmt(Flow *f, const PlumStmt *s);
static void walk_function(Flow *f, const PlumBranch *b, const char *name);

static void walk_expr(Flow *f, const PlumExpr *e) {
    if (e == NULL) {
        return;
    }
    switch (e->kind) {
    case PLUM_EXPR_INTERP:
        for (size_t i = 0; i < e->as.interp.count; i++) {
            walk_expr(f, e->as.interp.parts[i].expr);
        }
        break;
    case PLUM_EXPR_LIST:
        for (size_t i = 0; i < e->as.list.count; i++) {
            walk_expr(f, e->as.list.items[i]);
        }
        break;
    case PLUM_EXPR_MAP:
        for (size_t i = 0; i < e->as.map.count; i++) {
            walk_expr(f, e->as.map.entries[i].key);
            walk_expr(f, e->as.map.entries[i].value);
        }
        break;
    case PLUM_EXPR_UNARY:
        walk_expr(f, e->as.unary.operand);
        break;
    case PLUM_EXPR_BINARY:
        walk_expr(f, e->as.binary.left);
        walk_expr(f, e->as.binary.right);
        break;
    case PLUM_EXPR_RANGE:
        walk_expr(f, e->as.range.low);
        walk_expr(f, e->as.range.high);
        break;
    case PLUM_EXPR_IS:
    case PLUM_EXPR_AS:
        walk_expr(f, e->as.cast.value);
        break;
    case PLUM_EXPR_CALL:
        walk_expr(f, e->as.call.callee);
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            walk_expr(f, e->as.call.args[i].value);
        }
        break;
    case PLUM_EXPR_INDEX:
        walk_expr(f, e->as.index.object);
        walk_expr(f, e->as.index.index);
        break;
    case PLUM_EXPR_FIELD:
        walk_expr(f, e->as.field.object);
        break;
    case PLUM_EXPR_LAMBDA: {
        /* A lambda is a new function: loops outside it don't count. */
        int outer = f->loop_depth;
        f->loop_depth = 0;
        if (e->as.lambda.block != NULL) {
            walk_stmt(f, e->as.lambda.block);
        } else {
            walk_expr(f, e->as.lambda.body);
        }
        f->loop_depth = outer;
        break;
    }
    case PLUM_EXPR_IF:
        walk_expr(f, e->as.if_.cond);
        walk_expr(f, e->as.if_.then);
        walk_expr(f, e->as.if_.otherwise);
        break;
    case PLUM_EXPR_TRY:
    case PLUM_EXPR_SPAWN:
        walk_expr(f, e->as.inner);
        break;
    case PLUM_EXPR_MATCH:
        walk_expr(f, e->as.match.subject);
        match_exhaustive(f, e, true);
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            walk_stmt(f, e->as.match.arms[i].body);
        }
        break;
    default:
        break;
    }
}

static void warn_unused(Flow *f, const PlumStmt *s) {
    const PlumSymbol *sym = s->symbol;
    if (sym != NULL && !sym->used && sym->name[0] != '_') {
        plum_diag_warning(f->diags, s->span, "W0502",
                          "'%s' is never used; remove it, or start its name with '_' to keep it",
                          sym->name);
    }
}

static void walk_block(Flow *f, const PlumStmt *block) {
    bool warned = false;
    for (size_t i = 0; i < block->as.block.count; i++) {
        const PlumStmt *s = block->as.block.stmts[i];
        walk_stmt(f, s);
        if (!warned && i + 1 < block->as.block.count && ends(f, s, true)) {
            plum_diag_warning(f->diags, block->as.block.stmts[i + 1]->span, "W0501",
                              "this code never runs: the line before always leaves this block");
            warned = true;
        }
    }
}

static void walk_stmt(Flow *f, const PlumStmt *s) {
    if (s == NULL) {
        return;
    }
    switch (s->kind) {
    case PLUM_STMT_EXPR:
    case PLUM_STMT_THROW:
    case PLUM_STMT_RETURN:
        walk_expr(f, s->as.expr);
        break;
    case PLUM_STMT_DECL:
        walk_expr(f, s->as.decl.value);
        warn_unused(f, s);
        break;
    case PLUM_STMT_ASSIGN:
        walk_expr(f, s->as.assign.target);
        walk_expr(f, s->as.assign.value);
        break;
    case PLUM_STMT_BLOCK:
        walk_block(f, s);
        break;
    case PLUM_STMT_IF:
    case PLUM_STMT_IF_PLANT:
        walk_expr(f, s->kind == PLUM_STMT_IF ? s->as.if_.cond : s->as.if_.value);
        walk_stmt(f, s->as.if_.then);
        walk_stmt(f, s->as.if_.otherwise);
        break;
    case PLUM_STMT_WHILE:
    case PLUM_STMT_FOR:
        walk_expr(f, s->kind == PLUM_STMT_WHILE ? s->as.while_.cond : s->as.for_.iterable);
        f->loop_depth++;
        walk_stmt(f, s->kind == PLUM_STMT_WHILE ? s->as.while_.body : s->as.for_.body);
        f->loop_depth--;
        break;
    case PLUM_STMT_BREAK:
    case PLUM_STMT_CONTINUE:
        if (f->loop_depth == 0) {
            plum_diag_error(f->diags, s->span, "E0502", "'%s' can only be used inside a loop",
                            s->kind == PLUM_STMT_BREAK ? "break" : "continue");
        }
        break;
    case PLUM_STMT_TRY:
        walk_stmt(f, s->as.try_.body);
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            walk_stmt(f, s->as.try_.catches[i].body);
        }
        walk_stmt(f, s->as.try_.finally);
        break;
    case PLUM_STMT_BRANCH:
        walk_function(f, &s->as.branch, s->as.branch.name);
        break;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
        for (size_t i = 0; i < s->as.tree.field_count; i++) {
            walk_expr(f, s->as.tree.fields[i].fallback);
        }
        for (size_t i = 0; i < s->as.tree.method_count; i++) {
            walk_function(f, &s->as.tree.methods[i], s->as.tree.methods[i].name);
        }
        break;
    case PLUM_STMT_KIND:
    case PLUM_STMT_SEED:
    case PLUM_STMT_ERROR:
        break;
    }
}

/* A function with a result must return it on every path. */
static void walk_function(Flow *f, const PlumBranch *b, const char *name) {
    int outer = f->loop_depth;
    f->loop_depth = 0;
    if (b->arrow != NULL) {
        walk_expr(f, b->arrow);
    } else if (b->body != NULL) {
        walk_stmt(f, b->body);
        bool returns_value = b->result != NULL && !(b->result->kind == PLUM_TYPE_NAME &&
                                                    strcmp(b->result->as.name.name, "none") == 0);
        if (returns_value && !ends(f, b->body, false)) {
            /* Point at the last statement: the return belongs after it. */
            const PlumStmt *last = b->body->as.block.count > 0
                                       ? b->body->as.block.stmts[b->body->as.block.count - 1]
                                       : b->body;
            plum_diag_error(f->diags, last->span, "E0501",
                            "'%s' doesn't return a value on every path; add a 'return' after "
                            "this",
                            name);
        }
    }
    f->loop_depth = outer;
}

void plum_check_flow(const PlumProgram *program, PlumDiagList *diags) {
    Flow f = {diags, 0};
    PlumStmt block;
    memset(&block, 0, sizeof block);
    block.kind = PLUM_STMT_BLOCK;
    block.as.block.stmts = program->stmts;
    block.as.block.count = program->count;
    walk_block(&f, &block);
}
