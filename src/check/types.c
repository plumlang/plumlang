/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check/types.h"

#include <stdio.h>
#include <string.h>

PlumTy *plum_ty_new(PlumTypes *types, PlumTyKind kind) {
    PlumTy *t = plum_arena_alloc_zeroed(types->arena, sizeof *t);
    t->kind = kind;
    return t;
}

void plum_types_init(PlumTypes *types, PlumArena *arena) {
    types->arena = arena;
    types->error = plum_ty_new(types, PLUM_TY_ERROR);
    types->unknown = plum_ty_new(types, PLUM_TY_UNKNOWN);
    types->none = plum_ty_new(types, PLUM_TY_NONE);
    types->boolean = plum_ty_new(types, PLUM_TY_BOOL);
    types->integer = plum_ty_new(types, PLUM_TY_INT);
    types->floating = plum_ty_new(types, PLUM_TY_FLOAT);
    types->str = plum_ty_new(types, PLUM_TY_STR);
    types->byte = plum_ty_new(types, PLUM_TY_BYTE);
    types->range = plum_ty_new(types, PLUM_TY_RANGE);
}

PlumTy *plum_ty_wrap(PlumTypes *types, PlumTyKind kind, PlumTy *elem) {
    if (kind == PLUM_TY_OPTIONAL && elem->kind == PLUM_TY_OPTIONAL) {
        return elem; /* T?? is T? */
    }
    PlumTy *t = plum_ty_new(types, kind);
    t->as.elem = elem;
    return t;
}

PlumTy *plum_ty_map(PlumTypes *types, PlumTy *key, PlumTy *value) {
    PlumTy *t = plum_ty_new(types, PLUM_TY_MAP);
    t->as.map.key = key;
    t->as.map.value = value;
    return t;
}

PlumTy *plum_ty_named(PlumTypes *types, PlumTyKind kind, PlumSymbol *decl) {
    PlumTy *t = plum_ty_new(types, kind);
    t->as.decl = decl;
    return t;
}

bool plum_ty_is_loose(const PlumTy *t) {
    return t == NULL || t->kind == PLUM_TY_ERROR || t->kind == PLUM_TY_UNKNOWN;
}

bool plum_ty_is_numeric(const PlumTy *t) {
    return t->kind == PLUM_TY_INT || t->kind == PLUM_TY_FLOAT || t->kind == PLUM_TY_BYTE;
}

bool plum_ty_equal(const PlumTy *a, const PlumTy *b) {
    if (plum_ty_is_loose(a) || plum_ty_is_loose(b)) {
        return true;
    }
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PLUM_TY_LIST:
    case PLUM_TY_SET:
    case PLUM_TY_OPTIONAL:
    case PLUM_TY_TASK:
    case PLUM_TY_CHANNEL:
        return plum_ty_equal(a->as.elem, b->as.elem);
    case PLUM_TY_MAP:
        return plum_ty_equal(a->as.map.key, b->as.map.key) &&
               plum_ty_equal(a->as.map.value, b->as.map.value);
    case PLUM_TY_FUNC:
        if (a->as.func.param_count != b->as.func.param_count ||
            !plum_ty_equal(a->as.func.result, b->as.func.result)) {
            return false;
        }
        for (size_t i = 0; i < a->as.func.param_count; i++) {
            if (!plum_ty_equal(a->as.func.params[i].ty, b->as.func.params[i].ty)) {
                return false;
            }
        }
        return true;
    case PLUM_TY_TREE:
    case PLUM_TY_TRAIT:
    case PLUM_TY_KIND:
        return a->as.decl == b->as.decl;
    default:
        return true;
    }
}

bool plum_tree_extends(const PlumSymbol *decl, const PlumSymbol *ancestor) {
    if (decl == ancestor) {
        return true;
    }
    if (decl->decl == NULL) {
        /* A built-in error tree extends Error. */
        return strcmp(ancestor->name, "Error") == 0 && ancestor->decl == NULL;
    }
    const PlumStmt *s = decl->decl;
    if (s->kind != PLUM_STMT_TREE && s->kind != PLUM_STMT_TRAIT) {
        return false;
    }
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        const PlumType *super = s->as.tree.supers[i];
        if (super->kind == PLUM_TYPE_NAME && super->as.name.symbol != NULL &&
            super->as.name.symbol != decl && plum_tree_extends(super->as.name.symbol, ancestor)) {
            return true;
        }
    }
    return false;
}

bool plum_ty_assignable(const PlumTy *from, const PlumTy *to) {
    if (plum_ty_is_loose(from) || plum_ty_is_loose(to)) {
        return true;
    }
    if (to->kind == PLUM_TY_FLOAT && (from->kind == PLUM_TY_INT || from->kind == PLUM_TY_BYTE)) {
        return true; /* the one implicit conversion */
    }
    if (to->kind == PLUM_TY_OPTIONAL) {
        if (from->kind == PLUM_TY_NONE) {
            return true;
        }
        if (from->kind == PLUM_TY_OPTIONAL) {
            return plum_ty_assignable(from->as.elem, to->as.elem);
        }
        return plum_ty_assignable(from, to->as.elem);
    }
    if (from->kind == PLUM_TY_TREE && (to->kind == PLUM_TY_TREE || to->kind == PLUM_TY_TRAIT)) {
        return plum_tree_extends(from->as.decl, to->as.decl);
    }
    if (from->kind == PLUM_TY_LIST && to->kind == PLUM_TY_LIST) {
        /* A list of Dogs where a list of Animals is read (spec 8.4). */
        return plum_ty_assignable(from->as.elem, to->as.elem) &&
               (plum_ty_equal(from->as.elem, to->as.elem) || from->as.elem->kind == PLUM_TY_TREE);
    }
    return plum_ty_equal(from, to);
}

/* Append to buf, tracking the length. */
static void put(char *buf, size_t size, size_t *len, const char *s) {
    size_t n = strlen(s);
    if (*len + n >= size) {
        n = *len + 1 < size ? size - *len - 1 : 0;
    }
    memcpy(buf + *len, s, n);
    *len += n;
    buf[*len] = '\0';
}

static void text(const PlumTy *t, char *buf, size_t size, size_t *len) {
    if (t == NULL) {
        put(buf, size, len, "none");
        return;
    }
    switch (t->kind) {
    case PLUM_TY_ERROR:
        put(buf, size, len, "<error>");
        return;
    case PLUM_TY_UNKNOWN:
        put(buf, size, len, "<unknown>");
        return;
    case PLUM_TY_NONE:
        put(buf, size, len, "none");
        return;
    case PLUM_TY_BOOL:
        put(buf, size, len, "bool");
        return;
    case PLUM_TY_INT:
        put(buf, size, len, "int");
        return;
    case PLUM_TY_FLOAT:
        put(buf, size, len, "float");
        return;
    case PLUM_TY_STR:
        put(buf, size, len, "str");
        return;
    case PLUM_TY_BYTE:
        put(buf, size, len, "byte");
        return;
    case PLUM_TY_RANGE:
        put(buf, size, len, "range");
        return;
    case PLUM_TY_LIST:
    case PLUM_TY_SET:
    case PLUM_TY_TASK:
    case PLUM_TY_CHANNEL:
        put(buf, size, len,
            t->kind == PLUM_TY_LIST   ? "list["
            : t->kind == PLUM_TY_SET  ? "set["
            : t->kind == PLUM_TY_TASK ? "Task["
                                      : "Channel[");
        text(t->as.elem, buf, size, len);
        put(buf, size, len, "]");
        return;
    case PLUM_TY_MAP:
        put(buf, size, len, "map[");
        text(t->as.map.key, buf, size, len);
        put(buf, size, len, ", ");
        text(t->as.map.value, buf, size, len);
        put(buf, size, len, "]");
        return;
    case PLUM_TY_OPTIONAL:
        if (t->as.elem->kind == PLUM_TY_FUNC) {
            put(buf, size, len, "(");
            text(t->as.elem, buf, size, len);
            put(buf, size, len, ")?");
        } else {
            text(t->as.elem, buf, size, len);
            put(buf, size, len, "?");
        }
        return;
    case PLUM_TY_FUNC:
        put(buf, size, len, "(");
        for (size_t i = 0; i < t->as.func.param_count; i++) {
            if (i > 0) {
                put(buf, size, len, ", ");
            }
            text(t->as.func.params[i].ty, buf, size, len);
        }
        put(buf, size, len, ") -> ");
        text(t->as.func.result, buf, size, len);
        return;
    case PLUM_TY_TREE:
    case PLUM_TY_TRAIT:
    case PLUM_TY_KIND:
        put(buf, size, len, t->as.decl->name);
        return;
    }
}

const char *plum_ty_text(const PlumTy *t, char *buf, size_t size) {
    size_t len = 0;
    buf[0] = '\0';
    text(t, buf, size, &len);
    return buf;
}
