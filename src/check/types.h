/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The types the checker works with (spec section 3): what a value is, as
 * opposed to PlumType, which is how a type is written in source.
 */
#ifndef PLUM_CHECK_TYPES_H
#define PLUM_CHECK_TYPES_H

#include "check/resolve.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum PlumTyKind {
    PLUM_TY_ERROR,   /* an expression that already failed: accepted everywhere, never reported */
    PLUM_TY_UNKNOWN, /* not checked yet (Stage 5 features): accepted everywhere */
    PLUM_TY_NONE,
    PLUM_TY_BOOL,
    PLUM_TY_INT,
    PLUM_TY_FLOAT,
    PLUM_TY_STR,
    PLUM_TY_BYTE,
    PLUM_TY_RANGE, /* a..b */
    PLUM_TY_LIST,
    PLUM_TY_MAP,
    PLUM_TY_SET,
    PLUM_TY_OPTIONAL,
    PLUM_TY_FUNC,
    PLUM_TY_TASK,
    PLUM_TY_CHANNEL,
    PLUM_TY_TREE,  /* an object of a tree */
    PLUM_TY_TRAIT, /* a value of any tree implementing the trait */
    PLUM_TY_KIND,  /* a value of a kind */
} PlumTyKind;

typedef struct PlumTyParam {
    const char *name; /* NULL for function types written as (A, B) -> R */
    PlumTy *ty;
    bool has_default;
} PlumTyParam;

struct PlumTy {
    PlumTyKind kind;
    union {
        PlumTy *elem; /* LIST, SET, OPTIONAL, TASK, CHANNEL */
        struct {
            PlumTy *key;
            PlumTy *value;
        } map;
        struct {
            PlumTyParam *params;
            size_t param_count;
            PlumTy *result;
        } func;
        PlumSymbol *decl; /* TREE, TRAIT, KIND */
    } as;
};

/* The shared basic types. */
typedef struct PlumTypes {
    PlumArena *arena;
    PlumTy *error, *unknown, *none, *boolean, *integer, *floating, *str, *byte, *range;
} PlumTypes;

void plum_types_init(PlumTypes *types, PlumArena *arena);

PlumTy *plum_ty_new(PlumTypes *types, PlumTyKind kind);
PlumTy *plum_ty_wrap(PlumTypes *types, PlumTyKind kind, PlumTy *elem); /* list[T], T?, ... */
PlumTy *plum_ty_map(PlumTypes *types, PlumTy *key, PlumTy *value);
PlumTy *plum_ty_named(PlumTypes *types, PlumTyKind kind, PlumSymbol *decl);

/* ERROR or UNKNOWN: accepted anywhere, and nothing about it is reported. */
bool plum_ty_is_loose(const PlumTy *t);
bool plum_ty_is_numeric(const PlumTy *t); /* int, float, byte */
bool plum_ty_equal(const PlumTy *a, const PlumTy *b);

/*
 * Can a value of type `from` be used where `to` is expected? Equal types,
 * int (or byte) to float, T and none to T?, a tree to its parent trees and
 * traits, and anything loose.
 */
bool plum_ty_assignable(const PlumTy *from, const PlumTy *to);

/* Does tree `decl` extend `ancestor` (a tree or a trait), directly or not? */
bool plum_tree_extends(const PlumSymbol *decl, const PlumSymbol *ancestor);

/* Write a type as it is spelled in source ("list[str]", "int?") into buf. */
const char *plum_ty_text(const PlumTy *t, char *buf, size_t size);

#endif /* PLUM_CHECK_TYPES_H */
