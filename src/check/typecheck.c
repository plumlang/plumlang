/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check/typecheck.h"

#include "util/alloc.h"
#include "util/array.h"
#include "vm/methods.h"

#include <stdarg.h>
#include <string.h>

typedef struct Checker {
    PlumTypes t;
    PlumArena *arena;
    PlumInterner *names;
    PlumDiagList *diags;
    PlumTy *result;           /* the current function's result type; NULL at top level */
    PlumSymbol *self_tree;    /* the tree whose method is being checked */
    const PlumExpr *range_ok; /* the one range expression allowed right now */
} Checker;

#define TYBUF 96

static PlumTy *check_expr(Checker *c, PlumExpr *e, PlumTy *expected);
static void check_stmt(Checker *c, PlumStmt *s);
static PlumTy *from_syntax(Checker *c, const PlumType *type);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *tx(const PlumTy *t, char *buf) {
    return plum_ty_text(t, buf, TYBUF);
}

/* "expected int, found str" at `span`, unless either side is loose. */
static void mismatch(Checker *c, PlumSpan span, const PlumTy *expected, const PlumTy *found) {
    if (plum_ty_is_loose(expected) || plum_ty_is_loose(found)) {
        return;
    }
    char a[TYBUF], b[TYBUF];
    if (found->kind == PLUM_TY_OPTIONAL && plum_ty_assignable(found->as.elem, expected)) {
        plum_diag_error(c->diags, span, "E0408",
                        "expected %s, found %s, which may be none; use '?\?' to give a default "
                        "or 'if plant' to check it first",
                        tx(expected, a), tx(found, b));
        return;
    }
    if (found->kind == PLUM_TY_FLOAT && expected->kind == PLUM_TY_INT) {
        plum_diag_error(c->diags, span, "E0402",
                        "expected int, found float; convert with int(...) (it rounds toward "
                        "zero)");
        return;
    }
    plum_diag_error(c->diags, span, "E0402", "expected %s, found %s", tx(expected, a),
                    tx(found, b));
}

/* Check `e` against `expected` and report a mismatch. Returns e's type. */
static PlumTy *expect_type(Checker *c, PlumExpr *e, PlumTy *expected) {
    PlumTy *t = check_expr(c, e, expected);
    if (!plum_ty_assignable(t, expected)) {
        mismatch(c, e->span, expected, t);
    }
    return t;
}

static const char *name_of(const PlumExpr *e) {
    if (e->kind == PLUM_EXPR_NAME) {
        return e->as.name;
    }
    if (e->kind == PLUM_EXPR_FIELD) {
        return e->as.field.name;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Types written in source                                             */
/* ------------------------------------------------------------------ */

static PlumTy *builtin_named(Checker *c, const char *name) {
    if (strcmp(name, "int") == 0)
        return c->t.integer;
    if (strcmp(name, "float") == 0)
        return c->t.floating;
    if (strcmp(name, "bool") == 0)
        return c->t.boolean;
    if (strcmp(name, "str") == 0)
        return c->t.str;
    if (strcmp(name, "byte") == 0)
        return c->t.byte;
    if (strcmp(name, "none") == 0)
        return c->t.none;
    return NULL;
}

/* The type a type symbol names, with its generic arguments. */
static PlumTy *named_type(Checker *c, PlumSymbol *sym, PlumTy **args, size_t n, PlumSpan span) {
    switch (sym->kind) {
    case PLUM_SYM_TREE:
        return plum_ty_named(&c->t, PLUM_TY_TREE, sym);
    case PLUM_SYM_TRAIT:
        return plum_ty_named(&c->t, PLUM_TY_TRAIT, sym);
    case PLUM_SYM_KIND:
        return plum_ty_named(&c->t, PLUM_TY_KIND, sym);
    case PLUM_SYM_BUILTIN_TYPE:
        break;
    default:
        return c->t.error;
    }
    PlumTy *basic = builtin_named(c, sym->name);
    if (basic != NULL) {
        if (n > 0) {
            plum_diag_error(c->diags, span, "E0414", "'%s' takes no type arguments", sym->name);
        }
        return basic;
    }
    size_t want = strcmp(sym->name, "map") == 0 ? 2 : 1;
    if (n != want) {
        plum_diag_error(c->diags, span, "E0414", "'%s' needs %s, like %s", sym->name,
                        want == 2 ? "two types" : "one type",
                        want == 2 ? "map[str, int]" : "list[int]");
        return c->t.error;
    }
    if (want == 2) {
        return plum_ty_map(&c->t, args[0], args[1]);
    }
    PlumTyKind kind = strcmp(sym->name, "list") == 0   ? PLUM_TY_LIST
                      : strcmp(sym->name, "set") == 0  ? PLUM_TY_SET
                      : strcmp(sym->name, "Task") == 0 ? PLUM_TY_TASK
                                                       : PLUM_TY_CHANNEL;
    return plum_ty_wrap(&c->t, kind, args[0]);
}

static PlumTy *from_syntax(Checker *c, const PlumType *type) {
    if (type == NULL) {
        return c->t.none;
    }
    switch (type->kind) {
    case PLUM_TYPE_NAME: {
        if (type->as.name.symbol == NULL) {
            return c->t.error; /* reported by the resolver */
        }
        PlumTy *args[2] = {NULL, NULL};
        size_t n = type->as.name.arg_count;
        for (size_t i = 0; i < n && i < 2; i++) {
            args[i] = from_syntax(c, type->as.name.args[i]);
        }
        return named_type(c, type->as.name.symbol, args, n, type->span);
    }
    case PLUM_TYPE_OPTIONAL:
        return plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, from_syntax(c, type->as.optional));
    case PLUM_TYPE_FUNC: {
        PlumTy *f = plum_ty_new(&c->t, PLUM_TY_FUNC);
        size_t n = type->as.func.param_count;
        f->as.func.params = plum_arena_alloc_zeroed(c->arena, (n ? n : 1) * sizeof(PlumTyParam));
        for (size_t i = 0; i < n; i++) {
            f->as.func.params[i].ty = from_syntax(c, type->as.func.params[i]);
        }
        f->as.func.param_count = n;
        f->as.func.result = from_syntax(c, type->as.func.result);
        return f;
    }
    case PLUM_TYPE_ERROR:
        break;
    }
    return c->t.error;
}

/* The function type of a branch declaration (its signature). */
static PlumTy *signature(Checker *c, const PlumBranch *b) {
    PlumTy *f = plum_ty_new(&c->t, PLUM_TY_FUNC);
    size_t n = b->param_count;
    f->as.func.params = plum_arena_alloc_zeroed(c->arena, (n ? n : 1) * sizeof(PlumTyParam));
    for (size_t i = 0; i < n; i++) {
        f->as.func.params[i].name = b->params[i].name;
        f->as.func.params[i].ty = from_syntax(c, b->params[i].type);
        f->as.func.params[i].has_default = b->params[i].fallback != NULL;
    }
    f->as.func.param_count = n;
    f->as.func.result = from_syntax(c, b->result);
    return f;
}

/* ------------------------------------------------------------------ */
/* Members of trees, traits and kinds                                  */
/* ------------------------------------------------------------------ */

/* The parent tree of a tree, if it has one (traits don't count). */
static PlumSymbol *parent_tree(const PlumSymbol *tree) {
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

static const PlumField *find_field(const PlumSymbol *tree, const char *name) {
    for (const PlumSymbol *t = tree; t != NULL; t = parent_tree(t)) {
        if (t->decl == NULL) {
            continue;
        }
        const PlumStmt *s = t->decl;
        for (size_t i = 0; i < s->as.tree.field_count; i++) {
            if (s->as.tree.fields[i].name == name) {
                return &s->as.tree.fields[i];
            }
        }
    }
    return NULL;
}

/* A method of a tree or trait, searching parents and the traits it names. */
static const PlumBranch *find_method(const PlumSymbol *decl, const char *name, int depth) {
    if (decl == NULL || decl->decl == NULL || depth > 32) {
        return NULL;
    }
    const PlumStmt *s = decl->decl;
    if (s->kind != PLUM_STMT_TREE && s->kind != PLUM_STMT_TRAIT) {
        return NULL;
    }
    for (size_t i = 0; i < s->as.tree.method_count; i++) {
        if (s->as.tree.methods[i].name == name) {
            return &s->as.tree.methods[i];
        }
    }
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        const PlumType *super = s->as.tree.supers[i];
        if (super->kind == PLUM_TYPE_NAME && super->as.name.symbol != NULL) {
            const PlumBranch *m = find_method(super->as.name.symbol, name, depth + 1);
            if (m != NULL) {
                return m;
            }
        }
    }
    return NULL;
}

/* The built-in error trees all have `message: str`. */
static bool is_builtin_error(const PlumSymbol *tree) {
    return tree->kind == PLUM_SYM_TREE && tree->decl == NULL;
}

static bool is_error_tree(const PlumTy *t) {
    if (t->kind != PLUM_TY_TREE) {
        return false;
    }
    for (const PlumSymbol *s = t->as.decl; s != NULL; s = parent_tree(s)) {
        if (is_builtin_error(s)) {
            return true;
        }
    }
    return false;
}

static const PlumCase *find_case(const PlumSymbol *kind, const char *name) {
    const PlumStmt *s = kind->decl;
    if (s == NULL || s->kind != PLUM_STMT_KIND) {
        return NULL;
    }
    for (size_t i = 0; i < s->as.kind.case_count; i++) {
        if (s->as.kind.cases[i].name == name) {
            return &s->as.kind.cases[i];
        }
    }
    return NULL;
}

/* A kind case with values is a function that makes the kind. */
static PlumTy *case_type(Checker *c, PlumSymbol *kind, const PlumCase *cs) {
    PlumTy *k = plum_ty_named(&c->t, PLUM_TY_KIND, kind);
    if (cs->param_count == 0) {
        return k;
    }
    PlumTy *f = plum_ty_new(&c->t, PLUM_TY_FUNC);
    f->as.func.params = plum_arena_alloc_zeroed(c->arena, cs->param_count * sizeof(PlumTyParam));
    for (size_t i = 0; i < cs->param_count; i++) {
        f->as.func.params[i].name = cs->params[i].name;
        f->as.func.params[i].ty = from_syntax(c, cs->params[i].type);
    }
    f->as.func.param_count = cs->param_count;
    f->as.func.result = k;
    return f;
}

/* Built-in members we already know: task.wait(), channel.send(x) ... */
static PlumTy *builtin_member(Checker *c, PlumTy *obj, const char *name) {
    PlumTy *f = plum_ty_new(&c->t, PLUM_TY_FUNC);
    f->as.func.params = plum_arena_alloc_zeroed(c->arena, sizeof(PlumTyParam));
    f->as.func.result = c->t.none;
    if (obj->kind == PLUM_TY_TASK && strcmp(name, "wait") == 0) {
        f->as.func.result = obj->as.elem;
        return f;
    }
    if (obj->kind == PLUM_TY_CHANNEL) {
        if (strcmp(name, "send") == 0) {
            f->as.func.params[0].name = plum_intern_cstr(c->names, "value");
            f->as.func.params[0].ty = obj->as.elem;
            f->as.func.param_count = 1;
            return f;
        }
        if (strcmp(name, "close") == 0) {
            return f;
        }
        if (strcmp(name, "receive") == 0) {
            f->as.func.result = plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, obj->as.elem);
            return f;
        }
    }
    return NULL;
}

/* A function type with up to two named parameters. */
static PlumTy *method_type(Checker *c, PlumTy *result, const char *n1, PlumTy *t1, bool d1,
                           const char *n2, PlumTy *t2) {
    PlumTy *f = plum_ty_new(&c->t, PLUM_TY_FUNC);
    f->as.func.params = plum_arena_alloc_zeroed(c->arena, 2 * sizeof(PlumTyParam));
    size_t n = 0;
    if (n1 != NULL) {
        f->as.func.params[n++] = (PlumTyParam){plum_intern_cstr(c->names, n1), t1, d1};
    }
    if (n2 != NULL) {
        f->as.func.params[n++] = (PlumTyParam){plum_intern_cstr(c->names, n2), t2, false};
    }
    f->as.func.param_count = n;
    f->as.func.result = result;
    return f;
}

/* The type of a list method (vm/methods.h), or NULL with an error. */
static PlumTy *list_method(Checker *c, PlumExpr *e, PlumTy *list, const char *name) {
    PlumTy *elem = list->as.elem;
    char a[TYBUF];
    int m = plum_method_find(PLUM_RECEIVER_LIST, name);
    switch (m) {
    case PLUM_METHOD_LIST_PUSH:
        return method_type(c, c->t.none, "value", elem, false, NULL, NULL);
    case PLUM_METHOD_LIST_POP:
        return method_type(c, elem, NULL, NULL, false, NULL, NULL);
    case PLUM_METHOD_LIST_INSERT:
        return method_type(c, c->t.none, "index", c->t.integer, false, "value", elem);
    case PLUM_METHOD_LIST_REMOVE_AT:
        return method_type(c, elem, "index", c->t.integer, false, NULL, NULL);
    case PLUM_METHOD_LIST_CLEAR:
    case PLUM_METHOD_LIST_REVERSE:
        return method_type(c, c->t.none, NULL, NULL, false, NULL, NULL);
    case PLUM_METHOD_LIST_SORT:
    case PLUM_METHOD_LIST_SORTED:
        if (!plum_ty_is_loose(elem) && !plum_ty_is_numeric(elem) && elem->kind != PLUM_TY_STR) {
            plum_diag_error(c->diags, e->span, "E0415",
                            "%s() orders numbers or strings, but this list holds %s", name,
                            tx(elem, a));
            return NULL;
        }
        return method_type(c, m == PLUM_METHOD_LIST_SORT ? c->t.none : list, NULL, NULL, false,
                           NULL, NULL);
    case PLUM_METHOD_LIST_CONTAINS:
        return method_type(c, c->t.boolean, "value", elem, false, NULL, NULL);
    case PLUM_METHOD_LIST_INDEX_OF:
        return method_type(c, plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, c->t.integer), "value", elem,
                           false, NULL, NULL);
    case PLUM_METHOD_LIST_JOIN:
        if (!plum_ty_is_loose(elem) && elem->kind != PLUM_TY_STR) {
            plum_diag_error(c->diags, e->span, "E0415",
                            "join() joins a list of str, but this list holds %s; make them text "
                            "first",
                            tx(elem, a));
            return NULL;
        }
        return method_type(c, c->t.str, "separator", c->t.str, true, NULL, NULL);
    case PLUM_METHOD_LIST_FILTER:
    case PLUM_METHOD_LIST_ANY:
    case PLUM_METHOD_LIST_ALL:
    case PLUM_METHOD_LIST_FIND: {
        PlumTy *test = method_type(c, c->t.boolean, "item", elem, false, NULL, NULL);
        PlumTy *result = m == PLUM_METHOD_LIST_FILTER ? list
                         : m == PLUM_METHOD_LIST_FIND ? plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, elem)
                                                      : c->t.boolean;
        return method_type(c, result, "test", test, false, NULL, NULL);
    }
    default:
        break;
    }
    if (strcmp(name, "enumerate") == 0) {
        plum_diag_error(c->diags, e->span, "E0415",
                        "enumerate() is used as the list of a for loop: 'for i, x in "
                        "items.enumerate()'");
        return NULL;
    }
    plum_diag_error(c->diags, e->span, "E0415", "%s has no method '%s'", tx(list, a), name);
    return NULL;
}

/* The type of a map or set method (vm/methods.h), or NULL with an error. */
static PlumTy *table_method(Checker *c, PlumExpr *e, PlumTy *t, const char *name) {
    char a[TYBUF];
    if (t->kind == PLUM_TY_MAP) {
        PlumTy *k = t->as.map.key, *v = t->as.map.value;
        switch (plum_method_find(PLUM_RECEIVER_MAP, name)) {
        case PLUM_METHOD_MAP_GET:
        case PLUM_METHOD_MAP_REMOVE:
            return method_type(c, plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, v), "key", k, false, NULL,
                               NULL);
        case PLUM_METHOD_MAP_KEYS:
            return method_type(c, plum_ty_wrap(&c->t, PLUM_TY_LIST, k), NULL, NULL, false, NULL,
                               NULL);
        case PLUM_METHOD_MAP_VALUES:
            return method_type(c, plum_ty_wrap(&c->t, PLUM_TY_LIST, v), NULL, NULL, false, NULL,
                               NULL);
        case PLUM_METHOD_MAP_CLEAR:
            return method_type(c, c->t.none, NULL, NULL, false, NULL, NULL);
        default:
            break;
        }
    } else {
        PlumTy *item = t->as.elem;
        switch (plum_method_find(PLUM_RECEIVER_SET, name)) {
        case PLUM_METHOD_SET_ADD:
            return method_type(c, c->t.none, "value", item, false, NULL, NULL);
        case PLUM_METHOD_SET_REMOVE:
        case PLUM_METHOD_SET_CONTAINS:
            return method_type(c, c->t.boolean, "value", item, false, NULL, NULL);
        case PLUM_METHOD_SET_CLEAR:
            return method_type(c, c->t.none, NULL, NULL, false, NULL, NULL);
        default:
            break;
        }
    }
    plum_diag_error(c->diags, e->span, "E0415", "%s has no method '%s'", tx(t, a), name);
    return NULL;
}

/* a.b: a field, a method, a kind case, or a built-in member. */
static PlumTy *check_field(Checker *c, PlumExpr *e) {
    PlumExpr *obj = e->as.field.object;
    const char *name = e->as.field.name;
    char a[TYBUF];

    /* Direction.North, Figure.Circle */
    if (obj->kind == PLUM_EXPR_NAME && obj->symbol != NULL && obj->symbol->kind == PLUM_SYM_KIND) {
        const PlumCase *cs = find_case(obj->symbol, name);
        if (cs == NULL) {
            plum_diag_error(c->diags, e->span, "E0415", "the kind '%s' has no case '%s'",
                            obj->symbol->name, name);
            return c->t.error;
        }
        obj->ty = c->t.unknown;
        return case_type(c, obj->symbol, cs);
    }

    PlumTy *t = check_expr(c, obj, NULL);
    if (plum_ty_is_loose(t)) {
        return c->t.unknown;
    }
    bool optional = false;
    if (t->kind == PLUM_TY_OPTIONAL) {
        if (!e->as.field.optional) {
            const char *n = name_of(obj);
            plum_diag_error(c->diags, obj->span, "E0408",
                            "%s%s%s may be none; write '?.%s' to skip it when it is none, or "
                            "check it first with 'if plant'",
                            n ? "'" : "this value", n ? n : "", n ? "'" : "", name);
            return c->t.error;
        }
        optional = true;
        t = t->as.elem;
    }

    PlumTy *result = NULL;
    if (t->kind == PLUM_TY_TREE || t->kind == PLUM_TY_TRAIT) {
        if (t->kind == PLUM_TY_TREE && is_builtin_error(t->as.decl) &&
            strcmp(name, "message") == 0) {
            result = c->t.str;
        } else {
            const PlumField *f = t->kind == PLUM_TY_TREE ? find_field(t->as.decl, name) : NULL;
            const PlumBranch *m = f == NULL ? find_method(t->as.decl, name, 0) : NULL;
            if (f == NULL && m == NULL && t->kind == PLUM_TY_TREE) {
                /* A user tree extending a built-in error also has message. */
                if (is_error_tree(t) && strcmp(name, "message") == 0) {
                    result = c->t.str;
                }
            }
            if (f != NULL) {
                result = from_syntax(c, f->type);
            } else if (m != NULL) {
                result = signature(c, m);
            } else if (result == NULL) {
                plum_diag_error(c->diags, e->span, "E0415", "'%s' has no field or method '%s'",
                                tx(t, a), name);
                return c->t.error;
            }
        }
    } else if (t->kind == PLUM_TY_TASK || t->kind == PLUM_TY_CHANNEL) {
        result = builtin_member(c, t, name);
        if (result == NULL) {
            plum_diag_error(c->diags, e->span, "E0415", "'%s' has no method '%s'", tx(t, a), name);
            return c->t.error;
        }
    } else if (t->kind == PLUM_TY_LIST) {
        result = list_method(c, e, t, name);
        if (result == NULL) {
            return c->t.error;
        }
    } else if (t->kind == PLUM_TY_MAP || t->kind == PLUM_TY_SET) {
        result = table_method(c, e, t, name);
        if (result == NULL) {
            return c->t.error;
        }
    } else {
        /* Methods of str and numbers arrive with the standard library
         * (#24); accept them for now. */
        result = c->t.unknown;
    }
    return optional && !plum_ty_is_loose(result) ? plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, result)
                                                 : result;
}

/*
 * Match a call's arguments to parameters: positional first, then named,
 * then defaults. Reports missing, extra and mistyped arguments.
 */
static void check_args(Checker *c, const PlumExpr *call, const char *what,
                       const PlumTyParam *params, size_t count) {
    bool given[64] = {false};
    size_t n_args = call->as.call.arg_count;
    size_t positional = 0;
    if (count > 64) {
        count = 64;
    }

    for (size_t i = 0; i < n_args; i++) {
        PlumArg *arg = &call->as.call.args[i];
        size_t slot = count;
        if (arg->name == NULL) {
            slot = positional++;
            if (slot >= count) {
                plum_diag_error(c->diags, arg->span, "E0406",
                                "%s takes %zu argument%s, but %zu were given", what, count,
                                count == 1 ? "" : "s", n_args);
                check_expr(c, arg->value, NULL);
                continue;
            }
        } else {
            for (size_t k = 0; k < count; k++) {
                if (params[k].name == arg->name) {
                    slot = k;
                }
            }
            if (slot == count) {
                plum_diag_error(c->diags, arg->span, "E0407", "%s has no parameter named '%s'",
                                what, arg->name);
                check_expr(c, arg->value, NULL);
                continue;
            }
            if (given[slot]) {
                plum_diag_error(c->diags, arg->span, "E0407", "'%s' is given twice", arg->name);
            }
        }
        given[slot] = true;
        PlumTy *t = check_expr(c, arg->value, params[slot].ty);
        if (!plum_ty_assignable(t, params[slot].ty)) {
            char a[TYBUF], b[TYBUF];
            if (params[slot].name != NULL && !plum_ty_is_loose(t)) {
                plum_diag_error(
                    c->diags, arg->value->span, t->kind == PLUM_TY_OPTIONAL ? "E0408" : "E0402",
                    "'%s' expects %s, found %s%s", params[slot].name, tx(params[slot].ty, a),
                    tx(t, b), t->kind == PLUM_TY_OPTIONAL ? "; use '?\?' for a default" : "");
            } else {
                mismatch(c, arg->value->span, params[slot].ty, t);
            }
        }
    }

    for (size_t k = 0; k < count; k++) {
        if (!given[k] && !params[k].has_default) {
            if (params[k].name != NULL) {
                plum_diag_error(c->diags, call->span, "E0406", "%s is missing the argument '%s'",
                                what, params[k].name);
            } else {
                plum_diag_error(c->diags, call->span, "E0406",
                                "%s takes %zu argument%s, but %zu were given", what, count,
                                count == 1 ? "" : "s", n_args);
            }
            return;
        }
    }
}

static void args_loose(Checker *c, PlumExpr *call) {
    for (size_t i = 0; i < call->as.call.arg_count; i++) {
        check_expr(c, call->as.call.args[i].value, NULL);
    }
}

/* drop, ask, len, type_of, assert, exit */
static PlumTy *call_builtin_func(Checker *c, PlumExpr *call, const char *name) {
    size_t n = call->as.call.arg_count;
    PlumArg *args = call->as.call.args;
    for (size_t i = 0; i < n; i++) {
        if (args[i].name != NULL && strcmp(name, "ask") != 0 && strcmp(name, "assert") != 0 &&
            strcmp(name, "exit") != 0) {
            plum_diag_error(c->diags, args[i].span, "E0407", "%s() takes no named arguments", name);
        }
    }
    if (strcmp(name, "drop") == 0 || strcmp(name, "type_of") == 0) {
        args_loose(c, call);
        if (strcmp(name, "type_of") == 0 && n != 1) {
            plum_diag_error(c->diags, call->span, "E0406", "type_of() takes 1 argument");
        }
        return strcmp(name, "drop") == 0 ? c->t.none : c->t.str;
    }
    if (strcmp(name, "len") == 0) {
        if (n != 1) {
            plum_diag_error(c->diags, call->span, "E0406",
                            "len() takes 1 argument, but %zu were given", n);
            args_loose(c, call);
            return c->t.integer;
        }
        PlumTy *t = check_expr(c, args[0].value, NULL);
        if (!plum_ty_is_loose(t) && t->kind != PLUM_TY_STR && t->kind != PLUM_TY_LIST &&
            t->kind != PLUM_TY_MAP && t->kind != PLUM_TY_SET) {
            char a[TYBUF];
            plum_diag_error(c->diags, args[0].value->span,
                            t->kind == PLUM_TY_OPTIONAL ? "E0408" : "E0402",
                            "len() needs a str, list, map or set, found %s", tx(t, a));
        }
        return c->t.integer;
    }
    PlumTyParam p[2];
    memset(p, 0, sizeof p);
    if (strcmp(name, "ask") == 0) {
        p[0] = (PlumTyParam){plum_intern_cstr(c->names, "prompt"), c->t.str, true};
        check_args(c, call, "ask()", p, 1);
        return plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, c->t.str);
    }
    if (strcmp(name, "assert") == 0) {
        p[0] = (PlumTyParam){plum_intern_cstr(c->names, "condition"), c->t.boolean, false};
        p[1] = (PlumTyParam){plum_intern_cstr(c->names, "message"), c->t.str, true};
        check_args(c, call, "assert()", p, 2);
        return c->t.none;
    }
    /* exit */
    p[0] = (PlumTyParam){plum_intern_cstr(c->names, "code"), c->t.integer, true};
    check_args(c, call, "exit()", p, 1);
    return c->t.none;
}

/* int(x), float(x), str(x), byte(x), set(list) */
static PlumTy *call_conversion(Checker *c, PlumExpr *call, PlumSymbol *type) {
    const char *name = type->name;
    size_t n = call->as.call.arg_count;
    if (n != 1 || call->as.call.args[0].name != NULL) {
        plum_diag_error(c->diags, call->span, "E0406", "%s() takes 1 argument", name);
        args_loose(c, call);
        return builtin_named(c, name) != NULL ? builtin_named(c, name) : c->t.error;
    }
    PlumExpr *arg = call->as.call.args[0].value;
    PlumTy *t = check_expr(c, arg, NULL);
    char a[TYBUF];
    if (strcmp(name, "set") == 0) {
        if (t->kind == PLUM_TY_LIST) {
            return plum_ty_wrap(&c->t, PLUM_TY_SET, t->as.elem);
        }
        if (!plum_ty_is_loose(t)) {
            plum_diag_error(c->diags, arg->span, "E0416", "set() makes a set from a list, not a %s",
                            tx(t, a));
        }
        return plum_ty_wrap(&c->t, PLUM_TY_SET, c->t.unknown);
    }
    PlumTy *target = builtin_named(c, name);
    if (target == NULL || target->kind == PLUM_TY_NONE || target->kind == PLUM_TY_BOOL) {
        if (target != NULL && target->kind == PLUM_TY_BOOL) {
            plum_diag_error(c->diags, call->span, "E0416",
                            "bool(...) is not allowed; write the comparison you mean, like "
                            "'x != 0' or 'len(s) > 0'");
            return c->t.boolean;
        }
        plum_diag_error(c->diags, call->span, "E0405", "'%s' is a type and can't be called", name);
        return c->t.error;
    }
    if (plum_ty_is_loose(t) || target->kind == PLUM_TY_STR) {
        return target; /* str(x) works on every value */
    }
    bool ok = false;
    switch (target->kind) {
    case PLUM_TY_INT:
        ok = plum_ty_is_numeric(t) || t->kind == PLUM_TY_STR;
        break;
    case PLUM_TY_FLOAT:
        ok = plum_ty_is_numeric(t) || t->kind == PLUM_TY_STR;
        break;
    case PLUM_TY_BYTE:
        ok = t->kind == PLUM_TY_INT || t->kind == PLUM_TY_BYTE;
        break;
    default:
        break;
    }
    if (!ok) {
        plum_diag_error(c->diags, arg->span, t->kind == PLUM_TY_OPTIONAL ? "E0408" : "E0416",
                        "%s() can't convert %s%s", name, tx(t, a),
                        t->kind == PLUM_TY_OPTIONAL ? "; it may be none, so use '?\?' first" : "");
    }
    return target;
}

/* Tree(...): the fields in order (or init's parameters), named or not. */
static PlumTy *call_constructor(Checker *c, PlumExpr *call, PlumSymbol *tree) {
    PlumTy *object = plum_ty_named(&c->t, PLUM_TY_TREE, tree);
    char what[96];
    snprintf(what, sizeof what, "%s(...)", tree->name);
    if (is_builtin_error(tree)) {
        PlumTyParam p = {plum_intern_cstr(c->names, "message"), c->t.str, true};
        check_args(c, call, what, &p, 1);
        return object;
    }
    const PlumBranch *init = find_method(tree, plum_intern_cstr(c->names, "init"), 0);
    if (init != NULL) {
        PlumTy *sig = signature(c, init);
        check_args(c, call, what, sig->as.func.params, sig->as.func.param_count);
        return object;
    }
    /* The generated constructor: every field, parents' first. */
    PlumSymbol *chain[32];
    size_t depth = 0;
    for (PlumSymbol *t = tree; t != NULL && depth < 32; t = parent_tree(t)) {
        chain[depth++] = t;
    }
    PLUM_ARRAY(PlumTyParam) params = {0};
    for (size_t d = depth; d > 0; d--) {
        const PlumStmt *s = chain[d - 1]->decl;
        if (s == NULL) {
            /* extends a built-in error: it starts with message */
            PlumTyParam p = {plum_intern_cstr(c->names, "message"), c->t.str, true};
            plum_array_push(&params, p);
            continue;
        }
        for (size_t i = 0; i < s->as.tree.field_count; i++) {
            const PlumField *f = &s->as.tree.fields[i];
            PlumTyParam p = {f->name, from_syntax(c, f->type), f->fallback != NULL};
            plum_array_push(&params, p);
        }
    }
    check_args(c, call, what, params.items, params.len);
    plum_array_free(&params);
    return object;
}

/*
 * map, reduce and sorted_by on a list: their result depends on the
 * function given, so they are checked here. Returns NULL for any other call.
 */
static PlumTy *check_higher_order(Checker *c, PlumExpr *e) {
    PlumExpr *callee = e->as.call.callee;
    if (callee->kind != PLUM_EXPR_FIELD || callee->as.field.optional) {
        return NULL;
    }
    const char *name = callee->as.field.name;
    bool map = strcmp(name, "map") == 0, reduce = strcmp(name, "reduce") == 0,
         sorted_by = strcmp(name, "sorted_by") == 0;
    if (!map && !reduce && !sorted_by) {
        return NULL;
    }
    PlumTy *list = check_expr(c, callee->as.field.object, NULL);
    if (list->kind != PLUM_TY_LIST) {
        return NULL; /* reported (or not a list): the general path takes over */
    }
    PlumTy *elem = list->as.elem;
    size_t want = reduce ? 2 : 1;
    char a[TYBUF], b[TYBUF];
    if (e->as.call.arg_count != want) {
        plum_diag_error(c->diags, e->span, "E0406", "%s() takes %zu argument%s", name, want,
                        want == 1 ? "" : "s");
        return c->t.error;
    }
    for (size_t i = 0; i < want; i++) {
        if (e->as.call.args[i].name != NULL) {
            plum_diag_error(c->diags, e->as.call.args[i].span, "E0407",
                            "%s() takes its arguments in order, without names", name);
        }
    }
    PlumExpr *fn = e->as.call.args[0].value;
    PlumTy *acc = NULL;
    if (reduce) {
        /* reduce(f, initial): the initial value's type is what f returns */
        acc = check_expr(c, e->as.call.args[1].value, NULL);
    }
    PlumTy *want_fn = reduce ? method_type(c, acc, "total", acc, false, "item", elem)
                             : method_type(c, c->t.unknown, "item", elem, false, NULL, NULL);
    PlumTy *f = check_expr(c, fn, want_fn);
    PlumTy *result = c->t.unknown;
    if (!plum_ty_is_loose(f)) {
        if (f->kind != PLUM_TY_FUNC) {
            plum_diag_error(c->diags, fn->span, "E0402", "%s() needs a function, found %s", name,
                            tx(f, a));
            return c->t.error;
        }
        size_t params = f->as.func.param_count;
        if (params != (reduce ? 2u : 1u)) {
            plum_diag_error(c->diags, fn->span, "E0402",
                            "%s() needs a function of %s, but this one takes %zu parameter%s", name,
                            reduce ? "two values, (total, item)" : "one item", params,
                            params == 1 ? "" : "s");
            return c->t.error;
        }
        /* an annotated parameter must take the list's items */
        PlumTy *item_param = f->as.func.params[params - 1].ty;
        if (!plum_ty_assignable(elem, item_param)) {
            plum_diag_error(c->diags, fn->span, "E0402",
                            "%s()'s function takes %s, but the list holds %s", name,
                            tx(item_param, a), tx(elem, b));
            return c->t.error;
        }
        result = f->as.func.result;
    }
    PlumTy *out;
    if (map) {
        out = plum_ty_wrap(&c->t, PLUM_TY_LIST, result);
    } else if (reduce) {
        if (!plum_ty_assignable(result, acc)) {
            plum_diag_error(c->diags, fn->span, "E0402",
                            "reduce()'s function must return %s, like its initial value, but "
                            "returns %s",
                            tx(acc, a), tx(result, b));
        }
        out = acc;
    } else {
        if (!plum_ty_is_loose(result) && !plum_ty_is_numeric(result) &&
            result->kind != PLUM_TY_STR) {
            plum_diag_error(c->diags, fn->span, "E0415",
                            "sorted_by() orders by numbers or strings, but this key gives %s",
                            tx(result, a));
        }
        out = list;
    }
    /* The compiler reads the call's parameter count from the callee's type. */
    callee->ty = reduce ? method_type(c, out, "f", f, false, "initial", acc)
                        : method_type(c, out, "f", f, false, NULL, NULL);
    return out;
}

static PlumTy *check_call(Checker *c, PlumExpr *e) {
    PlumExpr *callee = e->as.call.callee;
    PlumSymbol *sym = callee->kind == PLUM_EXPR_NAME ? callee->symbol : NULL;
    char a[TYBUF];
    PlumTy *higher = check_higher_order(c, e);
    if (higher != NULL) {
        return higher;
    }

    if (sym != NULL) {
        switch (sym->kind) {
        case PLUM_SYM_BUILTIN_FUNC:
            callee->ty = c->t.unknown;
            return call_builtin_func(c, e, sym->name);
        case PLUM_SYM_BUILTIN_TYPE:
            callee->ty = c->t.unknown;
            return call_conversion(c, e, sym);
        case PLUM_SYM_TREE:
            callee->ty = c->t.unknown;
            return call_constructor(c, e, sym);
        case PLUM_SYM_TRAIT:
        case PLUM_SYM_KIND:
            plum_diag_error(c->diags, callee->span, "E0405", "'%s' is %s and can't be called; %s",
                            sym->name, plum_symbol_kind_text(sym->kind),
                            sym->kind == PLUM_SYM_TRAIT ? "create a tree that implements it"
                                                        : "use one of its cases, like Kind.Case");
            args_loose(c, e);
            return c->t.error;
        default:
            break;
        }
    }

    /* Channel[int]() */
    if (callee->kind == PLUM_EXPR_INDEX && callee->as.index.object->kind == PLUM_EXPR_NAME &&
        callee->as.index.object->symbol != NULL &&
        callee->as.index.object->symbol->kind == PLUM_SYM_BUILTIN_TYPE &&
        strcmp(callee->as.index.object->symbol->name, "Channel") == 0) {
        PlumExpr *arg = callee->as.index.index;
        PlumTy *elem = c->t.error;
        if (arg->kind == PLUM_EXPR_NAME && arg->symbol != NULL) {
            elem = named_type(c, arg->symbol, NULL, 0, arg->span);
        }
        args_loose(c, e);
        return plum_ty_wrap(&c->t, PLUM_TY_CHANNEL, elem);
    }

    PlumTy *f = check_expr(c, callee, NULL);
    if (plum_ty_is_loose(f)) {
        args_loose(c, e);
        return c->t.unknown;
    }
    if (f->kind == PLUM_TY_OPTIONAL && f->as.elem->kind == PLUM_TY_FUNC) {
        /* x?.method(): the call happens only when x is not none */
        PlumTy *inner = f->as.elem;
        const char *n = name_of(callee);
        check_args(c, e, n ? n : "this function", inner->as.func.params,
                   inner->as.func.param_count);
        return plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, inner->as.func.result);
    }
    if (f->kind != PLUM_TY_FUNC) {
        plum_diag_error(c->diags, callee->span, "E0405", "this is %s, not a function", tx(f, a));
        args_loose(c, e);
        return c->t.error;
    }
    char what[96];
    const char *n = name_of(callee);
    snprintf(what, sizeof what, n ? "'%s'" : "%s", n ? n : "this function");
    check_args(c, e, what, f->as.func.params, f->as.func.param_count);
    return f->as.func.result != NULL ? f->as.func.result : c->t.none;
}

/* ------------------------------------------------------------------ */
/* Operators                                                           */
/* ------------------------------------------------------------------ */

static bool is_int_like(const PlumTy *t) {
    return t->kind == PLUM_TY_INT || t->kind == PLUM_TY_BYTE;
}

/* The wider of two numeric types: float if either is, else int. */
static PlumTy *numeric_result(Checker *c, const PlumTy *a, const PlumTy *b) {
    return (a->kind == PLUM_TY_FLOAT || b->kind == PLUM_TY_FLOAT) ? c->t.floating : c->t.integer;
}

/* A hint for a condition that isn't a bool. */
static const char *truthiness_hint(const PlumTy *t) {
    switch (t->kind) {
    case PLUM_TY_INT:
    case PLUM_TY_FLOAT:
    case PLUM_TY_BYTE:
        return "compare it, like 'x != 0' or 'x > 0'";
    case PLUM_TY_STR:
        return "compare it, like 's != \"\"' or 'len(s) > 0'";
    case PLUM_TY_LIST:
    case PLUM_TY_MAP:
    case PLUM_TY_SET:
        return "check its length, like 'len(xs) > 0'";
    case PLUM_TY_OPTIONAL:
        return "check it with 'x != none' or unwrap it with 'if plant'";
    default:
        return "write the comparison you mean";
    }
}

static void expect_bool(Checker *c, PlumExpr *e, const char *where) {
    PlumTy *t = check_expr(c, e, c->t.boolean);
    if (!plum_ty_is_loose(t) && t->kind != PLUM_TY_BOOL) {
        char a[TYBUF];
        plum_diag_error(c->diags, e->span, "E0404", "%s must be a bool, but this is %s; %s", where,
                        tx(t, a), truthiness_hint(t));
    }
}

/*
 * The type of `left op right`, or an error at `span`. Shared by binary
 * expressions and compound assignment.
 */
static PlumTy *binary_type(Checker *c, PlumTokenKind op, PlumTy *l, PlumTy *r, PlumSpan span) {
    if (plum_ty_is_loose(l) || plum_ty_is_loose(r)) {
        switch (op) {
        case PLUM_TOK_EQ_EQ:
        case PLUM_TOK_BANG_EQ:
        case PLUM_TOK_LESS:
        case PLUM_TOK_LESS_EQ:
        case PLUM_TOK_GREATER:
        case PLUM_TOK_GREATER_EQ:
        case PLUM_TOK_AND:
        case PLUM_TOK_OR:
        case PLUM_TOK_IN:
            return c->t.boolean;
        case PLUM_TOK_SLASH:
            return c->t.floating;
        default:
            return c->t.unknown;
        }
    }
    const char *opt = plum_token_kind_text(op);
    char a[TYBUF], b[TYBUF];

    if (l->kind == PLUM_TY_OPTIONAL || r->kind == PLUM_TY_OPTIONAL) {
        if ((op == PLUM_TOK_EQ_EQ || op == PLUM_TOK_BANG_EQ) &&
            (l->kind == PLUM_TY_NONE || r->kind == PLUM_TY_NONE || plum_ty_assignable(l, r) ||
             plum_ty_assignable(r, l))) {
            return c->t.boolean; /* x == none, x != "a" */
        }
        plum_diag_error(c->diags, span, "E0408",
                        "'%s' can't use %s, which may be none; give a default with '?\?' or "
                        "check it with 'if plant' first",
                        opt, tx(l->kind == PLUM_TY_OPTIONAL ? l : r, a));
        return c->t.error;
    }

    switch (op) {
    case PLUM_TOK_PLUS:
        if (plum_ty_is_numeric(l) && plum_ty_is_numeric(r)) {
            return numeric_result(c, l, r);
        }
        if (l->kind == PLUM_TY_STR && r->kind == PLUM_TY_STR) {
            return c->t.str;
        }
        if (l->kind == PLUM_TY_LIST && r->kind == PLUM_TY_LIST && plum_ty_equal(l, r)) {
            return l;
        }
        if (l->kind == PLUM_TY_STR || r->kind == PLUM_TY_STR) {
            plum_diag_error(c->diags, span, "E0403",
                            "can't add %s and %s; to put a value in text, use interpolation: "
                            "\"text {value}\"",
                            tx(l, a), tx(r, b));
            return c->t.error;
        }
        break;
    case PLUM_TOK_MINUS:
    case PLUM_TOK_STAR:
        if (plum_ty_is_numeric(l) && plum_ty_is_numeric(r)) {
            return numeric_result(c, l, r);
        }
        if (op == PLUM_TOK_STAR && (l->kind == PLUM_TY_STR || r->kind == PLUM_TY_STR)) {
            plum_diag_error(c->diags, span, "E0403",
                            "can't multiply %s and %s; to repeat text use .repeat(n)", tx(l, a),
                            tx(r, b));
            return c->t.error;
        }
        break;
    case PLUM_TOK_SLASH:
        if (plum_ty_is_numeric(l) && plum_ty_is_numeric(r)) {
            return c->t.floating;
        }
        break;
    case PLUM_TOK_SLASH_SLASH:
    case PLUM_TOK_PERCENT:
    case PLUM_TOK_STAR_STAR:
        if (plum_ty_is_numeric(l) && plum_ty_is_numeric(r)) {
            return numeric_result(c, l, r);
        }
        break;
    case PLUM_TOK_AMP:
    case PLUM_TOK_PIPE:
    case PLUM_TOK_CARET:
    case PLUM_TOK_LESS_LESS:
    case PLUM_TOK_GREATER_GREATER:
        if (is_int_like(l) && is_int_like(r)) {
            return c->t.integer;
        }
        if (l->kind == PLUM_TY_BOOL && r->kind == PLUM_TY_BOOL &&
            (op == PLUM_TOK_AMP || op == PLUM_TOK_PIPE)) {
            plum_diag_error(c->diags, span, "E0403", "'%s' works on int bits; for bools use '%s'",
                            opt, op == PLUM_TOK_AMP ? "and" : "or");
            return c->t.error;
        }
        plum_diag_error(c->diags, span, "E0403", "'%s' needs int values, found %s and %s", opt,
                        tx(l, a), tx(r, b));
        return c->t.error;
    case PLUM_TOK_EQ_EQ:
    case PLUM_TOK_BANG_EQ:
        if ((plum_ty_is_numeric(l) && plum_ty_is_numeric(r)) || plum_ty_equal(l, r) ||
            plum_ty_assignable(l, r) || plum_ty_assignable(r, l)) {
            return c->t.boolean;
        }
        plum_diag_error(c->diags, span, "E0403",
                        "can't compare %s and %s with '%s'; they are never equal", tx(l, a),
                        tx(r, b), opt);
        return c->t.error;
    case PLUM_TOK_LESS:
    case PLUM_TOK_LESS_EQ:
    case PLUM_TOK_GREATER:
    case PLUM_TOK_GREATER_EQ:
        if ((plum_ty_is_numeric(l) && plum_ty_is_numeric(r)) ||
            (l->kind == PLUM_TY_STR && r->kind == PLUM_TY_STR)) {
            return c->t.boolean;
        }
        plum_diag_error(c->diags, span, "E0403",
                        "can't order %s and %s with '%s'; both sides must be numbers or both "
                        "strings",
                        tx(l, a), tx(r, b), opt);
        return c->t.error;
    case PLUM_TOK_AND:
    case PLUM_TOK_OR:
        return c->t.boolean; /* operands checked by the caller */
    case PLUM_TOK_IN:
        if ((r->kind == PLUM_TY_LIST || r->kind == PLUM_TY_SET) &&
            plum_ty_assignable(l, r->as.elem)) {
            return c->t.boolean;
        }
        if (r->kind == PLUM_TY_MAP && plum_ty_assignable(l, r->as.map.key)) {
            return c->t.boolean;
        }
        if (r->kind == PLUM_TY_STR && l->kind == PLUM_TY_STR) {
            return c->t.boolean;
        }
        if (r->kind == PLUM_TY_RANGE && is_int_like(l)) {
            return c->t.boolean;
        }
        plum_diag_error(c->diags, span, "E0403", "can't look for %s in %s", tx(l, a), tx(r, b));
        return c->t.error;
    default:
        break;
    }
    plum_diag_error(c->diags, span, "E0403", "'%s' can't be used on %s and %s", opt, tx(l, a),
                    tx(r, b));
    return c->t.error;
}

static PlumTy *check_binary(Checker *c, PlumExpr *e) {
    PlumTokenKind op = e->as.binary.op;
    if (op == PLUM_TOK_AND || op == PLUM_TOK_OR) {
        const char *where = op == PLUM_TOK_AND ? "each side of 'and'" : "each side of 'or'";
        expect_bool(c, e->as.binary.left, where);
        expect_bool(c, e->as.binary.right, where);
        return c->t.boolean;
    }
    if (op == PLUM_TOK_QUESTION_QUESTION) {
        PlumTy *l = check_expr(c, e->as.binary.left, NULL);
        if (plum_ty_is_loose(l)) {
            return check_expr(c, e->as.binary.right, NULL);
        }
        if (l->kind != PLUM_TY_OPTIONAL) {
            char a[TYBUF];
            plum_diag_error(c->diags, e->as.binary.left->span, "E0408",
                            "'?\?' gives a default for a value that may be none, but this is "
                            "always %s",
                            tx(l, a));
            check_expr(c, e->as.binary.right, NULL);
            return l;
        }
        PlumTy *r = check_expr(c, e->as.binary.right, l->as.elem);
        if (plum_ty_assignable(r, l->as.elem)) {
            return l->as.elem;
        }
        if (r->kind == PLUM_TY_OPTIONAL && plum_ty_assignable(r, l)) {
            return l; /* a ?? b where b may be none too */
        }
        mismatch(c, e->as.binary.right->span, l->as.elem, r);
        return l->as.elem;
    }
    PlumTy *l = check_expr(c, e->as.binary.left, NULL);
    if (op == PLUM_TOK_IN) {
        c->range_ok = e->as.binary.right; /* x in 0..10 */
    }
    PlumTy *r = check_expr(c, e->as.binary.right, NULL);
    return binary_type(c, op, l, r, e->span);
}

static PlumTy *check_unary(Checker *c, PlumExpr *e) {
    PlumTokenKind op = e->as.unary.op;
    if (op == PLUM_TOK_NOT) {
        expect_bool(c, e->as.unary.operand, "the value after 'not'");
        return c->t.boolean;
    }
    PlumTy *t = check_expr(c, e->as.unary.operand, NULL);
    if (plum_ty_is_loose(t)) {
        return t;
    }
    char a[TYBUF];
    if (op == PLUM_TOK_MINUS && plum_ty_is_numeric(t)) {
        return t->kind == PLUM_TY_FLOAT ? c->t.floating : c->t.integer;
    }
    if (op == PLUM_TOK_TILDE && is_int_like(t)) {
        return c->t.integer;
    }
    plum_diag_error(c->diags, e->span, "E0403", "'%s' can't be used on %s",
                    plum_token_kind_text(op), tx(t, a));
    return c->t.error;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

/*
 * One type for two values that must agree (list items, if branches):
 * equal types, int with float, or a value with none (making it optional).
 * Returns NULL if they can't agree.
 */
static PlumTy *unify(Checker *c, PlumTy *a, PlumTy *b) {
    if (plum_ty_is_loose(a)) {
        return b;
    }
    if (plum_ty_is_loose(b)) {
        return a;
    }
    if (plum_ty_is_numeric(a) && plum_ty_is_numeric(b) && a->kind != b->kind) {
        return c->t.floating;
    }
    if (plum_ty_assignable(b, a)) {
        return a;
    }
    if (plum_ty_assignable(a, b)) {
        return b;
    }
    if (a->kind == PLUM_TY_NONE) {
        return plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, b);
    }
    if (b->kind == PLUM_TY_NONE) {
        return plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, a);
    }
    return NULL;
}

static PlumTy *check_list(Checker *c, PlumExpr *e, PlumTy *expected) {
    PlumTy *want = expected != NULL && expected->kind == PLUM_TY_LIST ? expected->as.elem : NULL;
    if (e->as.list.count == 0) {
        if (want == NULL) {
            if (expected == NULL || !plum_ty_is_loose(expected)) {
                plum_diag_error(c->diags, e->span, "E0409",
                                "can't tell what this empty list holds; give the type, like "
                                "'plant xs: list[int] = []'");
            }
            return plum_ty_wrap(&c->t, PLUM_TY_LIST, c->t.error);
        }
        return expected;
    }
    PlumTy *elem = want;
    for (size_t i = 0; i < e->as.list.count; i++) {
        PlumExpr *item = e->as.list.items[i];
        PlumTy *t = check_expr(c, item, elem);
        if (elem == NULL) {
            elem = t;
            continue;
        }
        if (want != NULL) {
            if (!plum_ty_assignable(t, want)) {
                mismatch(c, item->span, want, t);
            }
            continue;
        }
        PlumTy *u = unify(c, elem, t);
        if (u == NULL) {
            char a[TYBUF], b[TYBUF];
            plum_diag_error(c->diags, item->span, "E0402",
                            "a list holds one type of value: this is %s, but the list holds %s",
                            tx(t, b), tx(elem, a));
        } else {
            elem = u;
        }
    }
    return plum_ty_wrap(&c->t, PLUM_TY_LIST, elem);
}

static PlumTy *check_map(Checker *c, PlumExpr *e, PlumTy *expected) {
    bool typed = expected != NULL && expected->kind == PLUM_TY_MAP;
    if (e->as.map.count == 0) {
        if (!typed) {
            if (expected == NULL || !plum_ty_is_loose(expected)) {
                plum_diag_error(c->diags, e->span, "E0409",
                                "can't tell what this empty map holds; give the type, like "
                                "'plant m: map[str, int] = {:}'");
            }
            return plum_ty_map(&c->t, c->t.error, c->t.error);
        }
        return expected;
    }
    PlumTy *key = typed ? expected->as.map.key : NULL;
    PlumTy *value = typed ? expected->as.map.value : NULL;
    for (size_t i = 0; i < e->as.map.count; i++) {
        PlumMapEntryExpr *entry = &e->as.map.entries[i];
        PlumTy *k = check_expr(c, entry->key, key);
        PlumTy *v = check_expr(c, entry->value, value);
        if (key == NULL) {
            key = k;
            value = v;
            if (!plum_ty_is_loose(k) && k->kind != PLUM_TY_INT && k->kind != PLUM_TY_STR &&
                k->kind != PLUM_TY_BOOL && k->kind != PLUM_TY_BYTE && k->kind != PLUM_TY_KIND) {
                char a[TYBUF];
                plum_diag_error(c->diags, entry->key->span, "E0402",
                                "map keys must be int, str, bool, byte or a kind, not %s",
                                tx(k, a));
            }
            continue;
        }
        if (!plum_ty_assignable(k, key)) {
            mismatch(c, entry->key->span, key, k);
        }
        if (typed) {
            if (!plum_ty_assignable(v, value)) {
                mismatch(c, entry->value->span, value, v);
            }
        } else {
            PlumTy *u = unify(c, value, v);
            if (u == NULL) {
                mismatch(c, entry->value->span, value, v);
            } else {
                value = u;
            }
        }
    }
    return plum_ty_map(&c->t, key, value);
}

static PlumTy *check_index(Checker *c, PlumExpr *e) {
    PlumTy *obj = check_expr(c, e->as.index.object, NULL);
    PlumExpr *idx = e->as.index.index;
    bool slice = idx->kind == PLUM_EXPR_RANGE;
    c->range_ok = idx;
    PlumTy *i = check_expr(c, idx, NULL);
    if (plum_ty_is_loose(obj)) {
        return c->t.unknown;
    }
    char a[TYBUF], b[TYBUF];
    switch (obj->kind) {
    case PLUM_TY_LIST:
    case PLUM_TY_STR:
        if (slice) {
            return obj;
        }
        if (!plum_ty_is_loose(i) && !is_int_like(i)) {
            plum_diag_error(c->diags, idx->span, "E0410", "an index into %s must be an int, not %s",
                            tx(obj, a), tx(i, b));
        }
        return obj->kind == PLUM_TY_STR ? c->t.str : obj->as.elem;
    case PLUM_TY_MAP:
        if (!plum_ty_assignable(i, obj->as.map.key)) {
            char k[TYBUF];
            plum_diag_error(c->diags, idx->span, "E0410", "the keys of %s are %s, not %s",
                            tx(obj, a), tx(obj->as.map.key, k), tx(i, b));
        }
        return obj->as.map.value;
    case PLUM_TY_OPTIONAL:
        plum_diag_error(c->diags, e->as.index.object->span, "E0408",
                        "this may be none; check it with 'if plant' before indexing");
        return c->t.error;
    default:
        plum_diag_error(c->diags, e->span, "E0410", "%s can't be indexed with [ ]", tx(obj, a));
        return c->t.error;
    }
}

static void check_function(Checker *c, PlumBranch *b, PlumTy *sig, PlumSymbol *self_tree);

static PlumTy *check_lambda(Checker *c, PlumExpr *e, PlumTy *expected) {
    size_t n = e->as.lambda.param_count;
    PlumTy *f = plum_ty_new(&c->t, PLUM_TY_FUNC);
    f->as.func.params = plum_arena_alloc_zeroed(c->arena, (n ? n : 1) * sizeof(PlumTyParam));
    f->as.func.param_count = n;
    bool context =
        expected != NULL && expected->kind == PLUM_TY_FUNC && expected->as.func.param_count == n;
    for (size_t i = 0; i < n; i++) {
        PlumParam *p = &e->as.lambda.params[i];
        PlumTy *t = p->type != NULL ? from_syntax(c, p->type)
                    : context       ? expected->as.func.params[i].ty
                                    : c->t.unknown;
        f->as.func.params[i].name = p->name;
        f->as.func.params[i].ty = t;
        if (p->symbol != NULL) {
            p->symbol->ty = t;
        }
    }
    PlumTy *outer_result = c->result;
    if (e->as.lambda.body != NULL) {
        c->result = NULL;
        f->as.func.result =
            check_expr(c, e->as.lambda.body, context ? expected->as.func.result : NULL);
    } else {
        f->as.func.result = context ? expected->as.func.result : c->t.unknown;
        c->result = f->as.func.result;
        PlumStmt *block = e->as.lambda.block;
        for (size_t i = 0; i < block->as.block.count; i++) {
            check_stmt(c, block->as.block.stmts[i]);
        }
    }
    c->result = outer_result;
    return f;
}

static PlumTy *check_if_expr(Checker *c, PlumExpr *e, PlumTy *expected) {
    expect_bool(c, e->as.if_.cond, "the condition of 'if'");
    PlumTy *a = check_expr(c, e->as.if_.then, expected);
    if (e->as.if_.otherwise == NULL) {
        return c->t.error; /* reported by the parser */
    }
    PlumTy *b = check_expr(c, e->as.if_.otherwise, expected);
    PlumTy *u = unify(c, a, b);
    if (u == NULL) {
        char x[TYBUF], y[TYBUF];
        plum_diag_error(c->diags, e->span, "E0402",
                        "the two branches give different types: %s and %s", tx(a, x), tx(b, y));
        return c->t.error;
    }
    return u;
}

/* A match pattern against the subject's type; binds names in case patterns. */
static void check_pattern(Checker *c, PlumExpr *p, PlumTy *subject) {
    char a[TYBUF], b[TYBUF];
    if (p->kind == PLUM_EXPR_WILDCARD) {
        return;
    }
    PlumSymbol *cs_sym = p->kind == PLUM_EXPR_NAME   ? p->symbol
                         : p->kind == PLUM_EXPR_CALL ? p->as.call.callee->symbol
                                                     : NULL;
    if (cs_sym != NULL && cs_sym->kind == PLUM_SYM_CASE) {
        PlumSymbol *kind = cs_sym->owner;
        if (!plum_ty_is_loose(subject) &&
            (subject->kind != PLUM_TY_KIND || subject->as.decl != kind)) {
            plum_diag_error(c->diags, p->span, "E0417",
                            "'%s' is a case of %s, but this match is on %s", cs_sym->name,
                            kind->name, tx(subject, a));
            return;
        }
        const PlumCase *cs = find_case(kind, cs_sym->name);
        size_t given = p->kind == PLUM_EXPR_CALL ? p->as.call.arg_count : 0;
        size_t want = cs != NULL ? cs->param_count : 0;
        if (given != want) {
            plum_diag_error(c->diags, p->span, "E0417",
                            "the case '%s' has %zu value%s, but the pattern names %zu",
                            cs_sym->name, want, want == 1 ? "" : "s", given);
        }
        for (size_t i = 0; i < given && i < want; i++) {
            PlumExpr *bind = p->as.call.args[i].value;
            if (bind->symbol != NULL) {
                bind->symbol->ty = from_syntax(c, cs->params[i].type);
            }
        }
        p->ty = subject;
        return;
    }
    c->range_ok = p;
    PlumTy *t = check_expr(c, p, subject);
    if (p->kind == PLUM_EXPR_RANGE) {
        if (!plum_ty_is_loose(subject) && !is_int_like(subject)) {
            plum_diag_error(c->diags, p->span, "E0417",
                            "a range pattern matches an int, but this match is on %s",
                            tx(subject, a));
        }
        return;
    }
    if (!plum_ty_assignable(t, subject) && !plum_ty_assignable(subject, t)) {
        plum_diag_error(c->diags, p->span, "E0417", "this pattern is %s, but the match is on %s",
                        tx(t, b), tx(subject, a));
    }
}

static PlumTy *check_match(Checker *c, PlumExpr *e, PlumTy *expected) {
    PlumTy *subject = check_expr(c, e->as.match.subject, NULL);
    PlumTy *result = NULL;
    bool is_value = true;
    for (size_t i = 0; i < e->as.match.arm_count; i++) {
        PlumMatchArm *arm = &e->as.match.arms[i];
        for (size_t k = 0; k < arm->pattern_count; k++) {
            check_pattern(c, arm->patterns[k], subject);
        }
        PlumTy *t;
        if (arm->body->kind == PLUM_STMT_EXPR) {
            t = check_expr(c, arm->body->as.expr, expected);
        } else {
            check_stmt(c, arm->body);
            is_value = false;
            t = c->t.none;
        }
        if (result == NULL) {
            result = t;
        } else if (is_value) {
            PlumTy *u = unify(c, result, t);
            result = u != NULL ? u : c->t.unknown; /* reported if used as a value */
        }
    }
    return is_value && result != NULL ? result : c->t.none;
}

static PlumTy *check_expr(Checker *c, PlumExpr *e, PlumTy *expected) {
    PlumTy *t = c->t.error;
    char a[TYBUF];
    switch (e->kind) {
    case PLUM_EXPR_INT:
        t = expected != NULL && expected->kind == PLUM_TY_BYTE && e->as.i >= 0 && e->as.i <= 255
                ? c->t.byte
                : c->t.integer;
        break;
    case PLUM_EXPR_FLOAT:
        t = c->t.floating;
        break;
    case PLUM_EXPR_STRING:
        t = c->t.str;
        break;
    case PLUM_EXPR_INTERP:
        for (size_t i = 0; i < e->as.interp.count; i++) {
            if (e->as.interp.parts[i].expr != NULL) {
                check_expr(c, e->as.interp.parts[i].expr, NULL);
            }
        }
        t = c->t.str;
        break;
    case PLUM_EXPR_BOOL:
        t = c->t.boolean;
        break;
    case PLUM_EXPR_NONE:
        t = c->t.none;
        break;
    case PLUM_EXPR_NAME: {
        PlumSymbol *sym = e->symbol;
        if (sym == NULL) {
            t = c->t.error;
        } else if (sym->ty != NULL) {
            t = sym->ty;
        } else if (sym->kind == PLUM_SYM_FUNC && sym->decl != NULL) {
            sym->ty = signature(c, &sym->decl->as.branch);
            t = sym->ty;
        } else {
            t = c->t.unknown; /* types, built-ins as values: checked where they are used */
        }
        break;
    }
    case PLUM_EXPR_SELF:
        t = c->self_tree != NULL ? plum_ty_named(&c->t, PLUM_TY_TREE, c->self_tree) : c->t.error;
        break;
    case PLUM_EXPR_SUPER: {
        PlumSymbol *parent = c->self_tree != NULL ? parent_tree(c->self_tree) : NULL;
        if (c->self_tree != NULL && parent == NULL) {
            plum_diag_error(c->diags, e->span, "E0415",
                            "'super' needs a parent tree, but %s has none", c->self_tree->name);
        }
        t = parent != NULL ? plum_ty_named(&c->t, PLUM_TY_TREE, parent) : c->t.error;
        break;
    }
    case PLUM_EXPR_LIST:
        t = check_list(c, e, expected);
        break;
    case PLUM_EXPR_MAP:
        t = check_map(c, e, expected);
        break;
    case PLUM_EXPR_UNARY:
        t = check_unary(c, e);
        break;
    case PLUM_EXPR_BINARY:
        t = check_binary(c, e);
        break;
    case PLUM_EXPR_RANGE: {
        if (e != c->range_ok) {
            plum_diag_error(c->diags, e->span, "E0418",
                            "a range can only be used in a for loop, a slice, with 'in' or as a "
                            "match pattern; it isn't a value of its own");
        }
        PlumTy *lo = check_expr(c, e->as.range.low, c->t.integer);
        PlumTy *hi = check_expr(c, e->as.range.high, c->t.integer);
        if ((!plum_ty_is_loose(lo) && !is_int_like(lo)) ||
            (!plum_ty_is_loose(hi) && !is_int_like(hi))) {
            char b[TYBUF];
            plum_diag_error(c->diags, e->span, "E0403", "a range needs int ends, found %s and %s",
                            tx(lo, a), tx(hi, b));
        }
        t = c->t.range;
        break;
    }
    case PLUM_EXPR_IS:
        check_expr(c, e->as.cast.value, NULL);
        from_syntax(c, e->as.cast.type);
        t = c->t.boolean;
        break;
    case PLUM_EXPR_AS: {
        PlumTy *from = check_expr(c, e->as.cast.value, NULL);
        PlumTy *to = from_syntax(c, e->as.cast.type);
        if (!plum_ty_is_loose(from) && !plum_ty_is_loose(to) &&
            !(plum_ty_is_numeric(from) && plum_ty_is_numeric(to))) {
            char b[TYBUF];
            plum_diag_error(c->diags, e->span, "E0416",
                            "'as' converts between numbers; for %s to %s use %s(...)", tx(from, a),
                            tx(to, b), tx(to, b));
        }
        t = to;
        break;
    }
    case PLUM_EXPR_CALL:
        t = check_call(c, e);
        break;
    case PLUM_EXPR_INDEX:
        t = check_index(c, e);
        break;
    case PLUM_EXPR_FIELD:
        t = check_field(c, e);
        break;
    case PLUM_EXPR_LAMBDA:
        t = check_lambda(c, e, expected);
        break;
    case PLUM_EXPR_IF:
        t = check_if_expr(c, e, expected);
        break;
    case PLUM_EXPR_TRY: {
        PlumTy *inner = check_expr(c, e->as.inner, NULL);
        t = plum_ty_is_loose(inner) || inner->kind == PLUM_TY_NONE
                ? inner
                : plum_ty_wrap(&c->t, PLUM_TY_OPTIONAL, inner);
        break;
    }
    case PLUM_EXPR_SPAWN:
        t = plum_ty_wrap(&c->t, PLUM_TY_TASK, check_expr(c, e->as.inner, NULL));
        break;
    case PLUM_EXPR_MATCH:
        t = check_match(c, e, expected);
        break;
    case PLUM_EXPR_WILDCARD:
    case PLUM_EXPR_ERROR:
        t = c->t.error;
        break;
    }
    e->ty = t;
    return t;
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

/* Spec 5.7: a value computed and thrown away is a mistake. */
static void check_unused(Checker *c, const PlumExpr *e) {
    switch (e->kind) {
    case PLUM_EXPR_CALL:
    case PLUM_EXPR_SPAWN:
    case PLUM_EXPR_MATCH:
    case PLUM_EXPR_ERROR:
        return;
    case PLUM_EXPR_TRY:
        if (e->as.inner->kind == PLUM_EXPR_CALL) {
            return;
        }
        break;
    case PLUM_EXPR_BINARY:
        if (e->as.binary.op == PLUM_TOK_EQ_EQ) {
            plum_diag_error(c->diags, e->span, "E0401",
                            "this comparison's result is not used; to assign, write '=' instead "
                            "of '=='");
            return;
        }
        break;
    default:
        break;
    }
    plum_diag_error(c->diags, e->span, "E0401",
                    "this value is not used; store it with 'plant name = ...' or pass it to a "
                    "function like drop(...)");
}

static void check_decl(Checker *c, PlumStmt *s) {
    PlumSymbol *sym = s->symbol;
    PlumTy *declared = s->as.decl.type != NULL ? from_syntax(c, s->as.decl.type) : NULL;
    PlumTy *value = c->t.error;
    if (s->as.decl.value != NULL) {
        value = check_expr(c, s->as.decl.value, declared);
    }
    PlumTy *t;
    if (declared != NULL) {
        if (!plum_ty_assignable(value, declared)) {
            mismatch(c, s->as.decl.value->span, declared, value);
        }
        t = declared;
    } else if (value->kind == PLUM_TY_NONE) {
        plum_diag_error(c->diags, s->as.decl.value->span, "E0409",
                        "can't tell the type of '%s' from none; write it, like 'plant %s: str? = "
                        "none'",
                        s->as.decl.name, s->as.decl.name);
        t = c->t.error;
    } else {
        t = value;
    }
    if (sym != NULL) {
        sym->ty = t;
    }
}

static void check_assign(Checker *c, PlumStmt *s) {
    PlumExpr *target = s->as.assign.target;
    PlumTy *tt = check_expr(c, target, NULL);
    PlumTokenKind op = s->as.assign.op;
    if (op == PLUM_TOK_EQ) {
        expect_type(c, s->as.assign.value, tt);
        return;
    }
    /* x op= y is x = x op y, and the result must still fit in x. */
    PlumTokenKind bin;
    switch (op) {
    case PLUM_TOK_PLUS_EQ:
        bin = PLUM_TOK_PLUS;
        break;
    case PLUM_TOK_MINUS_EQ:
        bin = PLUM_TOK_MINUS;
        break;
    case PLUM_TOK_STAR_EQ:
        bin = PLUM_TOK_STAR;
        break;
    case PLUM_TOK_SLASH_EQ:
        bin = PLUM_TOK_SLASH;
        break;
    case PLUM_TOK_SLASH_SLASH_EQ:
        bin = PLUM_TOK_SLASH_SLASH;
        break;
    case PLUM_TOK_PERCENT_EQ:
        bin = PLUM_TOK_PERCENT;
        break;
    case PLUM_TOK_STAR_STAR_EQ:
        bin = PLUM_TOK_STAR_STAR;
        break;
    case PLUM_TOK_AMP_EQ:
        bin = PLUM_TOK_AMP;
        break;
    case PLUM_TOK_PIPE_EQ:
        bin = PLUM_TOK_PIPE;
        break;
    case PLUM_TOK_CARET_EQ:
        bin = PLUM_TOK_CARET;
        break;
    case PLUM_TOK_LESS_LESS_EQ:
        bin = PLUM_TOK_LESS_LESS;
        break;
    default:
        bin = PLUM_TOK_GREATER_GREATER;
        break;
    }
    PlumTy *vt = check_expr(c, s->as.assign.value, NULL);
    PlumTy *rt = binary_type(c, bin, tt, vt, s->span);
    if (!plum_ty_assignable(rt, tt)) {
        char a[TYBUF], b[TYBUF];
        const char *n = name_of(target);
        if (op == PLUM_TOK_SLASH_EQ && tt->kind == PLUM_TY_INT) {
            plum_diag_error(c->diags, s->span, "E0402",
                            "'/=' makes a float, but '%s' is an int; use '//=' to divide whole "
                            "numbers",
                            n ? n : "the target");
        } else {
            plum_diag_error(c->diags, s->span, "E0402", "'%s' makes %s, but '%s' is %s",
                            plum_token_kind_text(op), tx(rt, a), n ? n : "the target", tx(tt, b));
        }
    }
}

/* Is `e` a call of xs.enumerate() on a list? */
static bool is_enumerate(const PlumExpr *e) {
    return e->kind == PLUM_EXPR_CALL && e->as.call.arg_count == 0 &&
           e->as.call.callee->kind == PLUM_EXPR_FIELD &&
           strcmp(e->as.call.callee->as.field.name, "enumerate") == 0;
}

static void check_for(Checker *c, PlumStmt *s) {
    PlumExpr *iterable = s->as.for_.iterable;
    if (is_enumerate(iterable)) {
        /* for i, x in items.enumerate(): a position and an item */
        PlumExpr *callee = iterable->as.call.callee;
        PlumTy *list = check_expr(c, callee->as.field.object, NULL);
        PlumTy *elem = c->t.unknown;
        if (list->kind == PLUM_TY_LIST) {
            elem = list->as.elem;
        } else if (!plum_ty_is_loose(list)) {
            char a[TYBUF];
            plum_diag_error(c->diags, callee->span, "E0412", "enumerate() needs a list, not %s",
                            tx(list, a));
        }
        callee->ty = c->t.unknown;
        iterable->ty = list;
        if (s->symbol != NULL) {
            s->symbol->ty = s->as.for_.second != NULL ? c->t.integer : elem;
        }
        if (s->symbol2 != NULL) {
            s->symbol2->ty = elem;
        }
        if (s->as.for_.second == NULL) {
            plum_diag_error(c->diags, s->span, "E0412",
                            "enumerate() gives a position and an item: write 'for i, x in "
                            "items.enumerate()', or 'for x in items' for the items alone");
        }
        check_stmt(c, s->as.for_.body);
        return;
    }
    c->range_ok = iterable;
    PlumTy *it = check_expr(c, iterable, NULL);
    PlumTy *first = c->t.unknown, *second = NULL;
    bool two = s->as.for_.second != NULL;
    char a[TYBUF];
    switch (it->kind) {
    case PLUM_TY_RANGE:
        first = c->t.integer;
        break;
    case PLUM_TY_LIST:
    case PLUM_TY_SET:
    case PLUM_TY_CHANNEL:
        first = it->as.elem;
        break;
    case PLUM_TY_STR:
        first = c->t.str;
        break;
    case PLUM_TY_MAP:
        first = it->as.map.key;
        second = it->as.map.value;
        break;
    case PLUM_TY_ERROR:
    case PLUM_TY_UNKNOWN:
        first = c->t.unknown;
        second = c->t.unknown;
        break;
    case PLUM_TY_OPTIONAL:
        plum_diag_error(c->diags, s->as.for_.iterable->span, "E0412",
                        "this may be none; check it with 'if plant' before looping over it");
        first = second = c->t.error;
        break;
    default:
        plum_diag_error(c->diags, s->as.for_.iterable->span, "E0412",
                        "can't loop over %s; loop over a range, a list, a set, a map or a str",
                        tx(it, a));
        first = second = c->t.error;
        break;
    }
    if (two && second == NULL) {
        plum_diag_error(c->diags, s->span, "E0412",
                        "two loop names need a map; for a position and a value write 'for i, x "
                        "in items.enumerate()'");
        second = c->t.error;
    }
    if (s->symbol != NULL) {
        s->symbol->ty = first;
    }
    if (s->symbol2 != NULL) {
        s->symbol2->ty = second;
    }
    check_stmt(c, s->as.for_.body);
}

static void check_return(Checker *c, PlumStmt *s) {
    PlumTy *want = c->result;
    PlumExpr *value = s->as.expr;
    if (want == NULL) {
        if (value != NULL) {
            check_expr(c, value, NULL); /* a lambda with a block body */
        }
        return;
    }
    if (plum_ty_is_loose(want)) {
        if (value != NULL) {
            check_expr(c, value, NULL);
        }
        return;
    }
    char a[TYBUF];
    if (want->kind == PLUM_TY_NONE) {
        if (value != NULL) {
            check_expr(c, value, NULL);
            plum_diag_error(c->diags, value->span, "E0411",
                            "this function returns nothing; to return a value, add '-> type' to "
                            "its signature");
        }
        return;
    }
    if (value == NULL) {
        plum_diag_error(c->diags, s->span, "E0411", "this function must return %s", tx(want, a));
        return;
    }
    expect_type(c, value, want);
}

static void check_catch_type(Checker *c, const PlumCatch *cat) {
    const PlumType *type = cat->type;
    PlumSymbol *binding = cat->symbol;
    PlumTy *t = type != NULL         ? from_syntax(c, type)
                : cat->error != NULL ? plum_ty_named(&c->t, PLUM_TY_TREE, cat->error)
                                     : c->t.unknown;
    if (!plum_ty_is_loose(t) && !is_error_tree(t)) {
        char a[TYBUF];
        plum_diag_error(c->diags, type->span, "E0413",
                        "only errors can be caught; %s doesn't extend Error", tx(t, a));
        t = c->t.error;
    }
    if (binding != NULL) {
        binding->ty = t;
    }
}

static void check_stmt(Checker *c, PlumStmt *s) {
    switch (s->kind) {
    case PLUM_STMT_EXPR:
        check_expr(c, s->as.expr, NULL);
        check_unused(c, s->as.expr);
        break;
    case PLUM_STMT_DECL:
        check_decl(c, s);
        break;
    case PLUM_STMT_ASSIGN:
        check_assign(c, s);
        break;
    case PLUM_STMT_BLOCK:
        for (size_t i = 0; i < s->as.block.count; i++) {
            check_stmt(c, s->as.block.stmts[i]);
        }
        break;
    case PLUM_STMT_IF:
        expect_bool(c, s->as.if_.cond, "the condition of 'if'");
        check_stmt(c, s->as.if_.then);
        if (s->as.if_.otherwise != NULL) {
            check_stmt(c, s->as.if_.otherwise);
        }
        break;
    case PLUM_STMT_IF_PLANT: {
        PlumTy *t = check_expr(c, s->as.if_.value, NULL);
        PlumTy *inner = c->t.error;
        if (t->kind == PLUM_TY_OPTIONAL) {
            inner = t->as.elem;
        } else if (!plum_ty_is_loose(t)) {
            char a[TYBUF];
            plum_diag_error(c->diags, s->as.if_.value->span, "E0408",
                            "'if plant' unwraps a value that may be none, but this is always %s; "
                            "use 'plant' instead",
                            tx(t, a));
            inner = t;
        }
        if (s->symbol != NULL) {
            s->symbol->ty = inner;
        }
        check_stmt(c, s->as.if_.then);
        if (s->as.if_.otherwise != NULL) {
            check_stmt(c, s->as.if_.otherwise);
        }
        break;
    }
    case PLUM_STMT_WHILE:
        expect_bool(c, s->as.while_.cond, "the condition of 'while'");
        check_stmt(c, s->as.while_.body);
        break;
    case PLUM_STMT_FOR:
        check_for(c, s);
        break;
    case PLUM_STMT_RETURN:
        check_return(c, s);
        break;
    case PLUM_STMT_THROW: {
        PlumTy *t = check_expr(c, s->as.expr, NULL);
        if (!plum_ty_is_loose(t) && !is_error_tree(t)) {
            char a[TYBUF];
            plum_diag_error(c->diags, s->as.expr->span, "E0413",
                            "only errors can be thrown, like ValueError(\"...\"); this is %s",
                            tx(t, a));
        }
        break;
    }
    case PLUM_STMT_TRY:
        check_stmt(c, s->as.try_.body);
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            PlumCatch *cat = &s->as.try_.catches[i];
            check_catch_type(c, cat);
            check_stmt(c, cat->body);
        }
        if (s->as.try_.finally != NULL) {
            check_stmt(c, s->as.try_.finally);
        }
        break;
    case PLUM_STMT_BRANCH:
        if (c->result != NULL || c->self_tree != NULL) {
            /* A function inside a function is checked where it stands. */
            PlumTy *sig = signature(c, &s->as.branch);
            if (s->symbol != NULL) {
                s->symbol->ty = sig;
            }
            check_function(c, &s->as.branch, sig, NULL);
        }
        break; /* module functions are checked after the top level */
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
    case PLUM_STMT_KIND:
    case PLUM_STMT_SEED:
    case PLUM_STMT_BREAK:
    case PLUM_STMT_CONTINUE:
    case PLUM_STMT_ERROR:
        break;
    }
}

static void check_function(Checker *c, PlumBranch *b, PlumTy *sig, PlumSymbol *self_tree) {
    PlumTy *outer_result = c->result;
    PlumSymbol *outer_self = c->self_tree;
    c->result = sig->as.func.result;
    if (self_tree != NULL) {
        c->self_tree = self_tree;
    }
    for (size_t i = 0; i < b->param_count; i++) {
        PlumParam *p = &b->params[i];
        if (p->symbol != NULL) {
            p->symbol->ty = sig->as.func.params[i].ty;
        }
        if (p->fallback != NULL) {
            expect_type(c, p->fallback, sig->as.func.params[i].ty);
        }
    }
    if (b->arrow != NULL) {
        PlumTy *want = sig->as.func.result;
        if (want->kind == PLUM_TY_NONE) {
            check_expr(c, b->arrow, NULL);
        } else {
            expect_type(c, b->arrow, want);
        }
    } else if (b->body != NULL) {
        check_stmt(c, b->body);
    }
    c->result = outer_result;
    c->self_tree = outer_self;
}

void plum_typecheck(const PlumProgram *program, PlumArena *arena, PlumInterner *names,
                    PlumDiagList *diags) {
    Checker c;
    memset(&c, 0, sizeof c);
    plum_types_init(&c.t, arena);
    c.arena = arena;
    c.names = names;
    c.diags = diags;

    /* 1. Signatures: every module function's type is known up front. */
    for (size_t i = 0; i < program->count; i++) {
        PlumStmt *s = program->stmts[i];
        if (s->kind == PLUM_STMT_BRANCH && s->symbol != NULL) {
            s->symbol->ty = signature(&c, &s->as.branch);
        }
    }
    /* 2. The top level, in order, giving module variables their types. */
    for (size_t i = 0; i < program->count; i++) {
        check_stmt(&c, program->stmts[i]);
    }
    /* 3. Function and method bodies, and field defaults. */
    for (size_t i = 0; i < program->count; i++) {
        PlumStmt *s = program->stmts[i];
        if (s->kind == PLUM_STMT_BRANCH && s->symbol != NULL) {
            check_function(&c, &s->as.branch, s->symbol->ty, NULL);
        } else if (s->kind == PLUM_STMT_TREE || s->kind == PLUM_STMT_TRAIT) {
            for (size_t k = 0; k < s->as.tree.field_count; k++) {
                PlumField *f = &s->as.tree.fields[k];
                if (f->fallback != NULL) {
                    expect_type(&c, f->fallback, from_syntax(&c, f->type));
                }
            }
            for (size_t k = 0; k < s->as.tree.method_count; k++) {
                PlumBranch *m = &s->as.tree.methods[k];
                check_function(&c, m, signature(&c, m), s->symbol);
            }
        }
    }
}
