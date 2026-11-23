/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Name resolution (spec sections 4.2, 6, 8 and 9.4): connect every name in
 * a program to the declaration it refers to, and report names that don't
 * exist, are declared twice, are used before their declaration, or can't
 * be changed.
 *
 * Scopes nest: the built-ins, the module, then functions and blocks. At the
 * top of a module, functions, trees, traits and kinds are visible from the
 * start of the file; variables only after their `plant` or `root` line.
 * Inside a function body every module-level name is visible, wherever it is
 * declared, as the body runs later.
 */
#ifndef PLUM_CHECK_RESOLVE_H
#define PLUM_CHECK_RESOLVE_H

#include "diag/diag.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "util/intern.h"
#include "util/map.h"

typedef enum PlumSymbolKind {
    PLUM_SYM_VAR,          /* plant */
    PLUM_SYM_CONST,        /* root */
    PLUM_SYM_PARAM,        /* a function or lambda parameter */
    PLUM_SYM_LOOP,         /* a for loop's name */
    PLUM_SYM_BINDING,      /* if plant x = ..., a catch variable, a pattern name */
    PLUM_SYM_FUNC,         /* branch at module level */
    PLUM_SYM_TREE,         /* tree, including the built-in error trees */
    PLUM_SYM_TRAIT,        /* trait */
    PLUM_SYM_KIND,         /* kind */
    PLUM_SYM_CASE,         /* a case of a kind */
    PLUM_SYM_BUILTIN_FUNC, /* drop, ask, len, ... */
    PLUM_SYM_BUILTIN_TYPE, /* int, str, list, ... (int, float, str, byte convert too) */
    PLUM_SYM_MODULE,       /* seed math: the module; decl is the seed statement */
    PLUM_SYM_NATIVE,       /* seed math { sqrt }: one of a module's functions or constants */
} PlumSymbolKind;

struct PlumSymbol {
    PlumSymbolKind kind;
    const char *name;   /* interned */
    PlumSpan span;      /* where it is declared (zero for built-ins) */
    PlumStmt *decl;     /* the declaring statement, when there is one */
    PlumSymbol *owner;  /* CASE: its kind */
    int function_depth; /* 0 at module level, +1 inside each function or lambda */
    bool global;        /* declared at module level */
    bool defined;       /* module variables: its declaration has been reached */
    bool captured;      /* a local used by a nested function or lambda */
    bool used;          /* referred to at least once */
    PlumTy *ty;         /* its type, filled in by the type checker */
    int slot;           /* the compiler's local or global slot */
    bool has_slot;      /* slot has been assigned */
    /* Trees and traits, for the type checker: 1 + the longest chain of
     * trees and traits it extends; 0 not measured yet, -1 while measuring. */
    int lineage;
};

/*
 * Resolve every name in `program`, filling in the symbol pointers of the
 * syntax tree. Errors go to `diags`. Symbols live in `arena`. If
 * `exports` isn't NULL, it receives the names other modules can import
 * (name -> PlumSymbol*).
 */
void plum_resolve(const PlumProgram *program, PlumArena *arena, PlumInterner *names,
                  PlumDiagList *diags, PlumMap *exports);

/*
 * The REPL: the names earlier entries declared (name -> PlumSymbol*) and
 * their kinds' cases, which each new entry sees.
 */
typedef struct PlumReplScope {
    PlumMap names;
    PlumMap cases;
} PlumReplScope;

void plum_repl_scope_init(PlumReplScope *scope);
/* Add (or replace) every name of `from` in `into`. */
void plum_repl_scope_merge(PlumReplScope *into, const PlumReplScope *from);
void plum_repl_scope_free(PlumReplScope *scope);

/*
 * Resolve one REPL entry, a program that sees the names in `earlier`. It
 * may declare one of them again, hiding the old one. The names it
 * declares go into `added` (an initialised, usually empty scope), for the
 * caller to merge into `earlier` once the entry has run.
 */
void plum_resolve_entry(const PlumProgram *program, PlumArena *arena, PlumInterner *names,
                        PlumDiagList *diags, const PlumReplScope *earlier, PlumReplScope *added);

/* The built-in names (types, functions and error trees): the i-th, with
 * its kind in *kind, or NULL past the end. */
const char *plum_builtin_at(size_t i, PlumSymbolKind *kind);

/* "a variable", "a function", "a tree", ... for messages. */
const char *plum_symbol_kind_text(PlumSymbolKind kind);

/*
 * The standard-library function a call's callee names: math.sqrt, sqrt
 * (from seed math { sqrt }), or a method of str (text.upper, once types
 * are known). NULL otherwise. `is_method` says the receiver goes first.
 */
struct PlumNativeFunction;
struct PlumNativeConstant;
const struct PlumNativeFunction *plum_callee_native(const PlumExpr *callee, bool *is_method);
/* The module constant an expression names (math.PI, or PI), or NULL. */
const struct PlumNativeConstant *plum_expr_native_constant(const PlumExpr *e);

#endif /* PLUM_CHECK_RESOLVE_H */
