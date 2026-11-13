/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check/resolve.h"

#include "util/alloc.h"
#include "util/map.h"

#include <ctype.h>
#include <string.h>

typedef enum ScopeKind {
    SCOPE_BUILTIN,
    SCOPE_MODULE,
    SCOPE_FUNCTION,
    SCOPE_BLOCK,
} ScopeKind;

typedef struct Scope {
    struct Scope *parent;
    ScopeKind kind;
    PlumMap names; /* name -> PlumSymbol* */
} Scope;

typedef struct Resolver {
    PlumArena *arena;
    PlumInterner *names;
    PlumDiagList *diags;
    Scope *scope;
    PlumMap cases;      /* bare kind-case names, for match patterns */
    int function_depth; /* 0 at module level */
    bool in_method;     /* inside a tree's method: self and super are allowed */
} Resolver;

const char *plum_symbol_kind_text(PlumSymbolKind kind) {
    switch (kind) {
    case PLUM_SYM_VAR:
        return "a variable";
    case PLUM_SYM_CONST:
        return "a root constant";
    case PLUM_SYM_PARAM:
        return "a parameter";
    case PLUM_SYM_LOOP:
        return "a loop variable";
    case PLUM_SYM_BINDING:
        return "a name bound by if plant, catch or a pattern";
    case PLUM_SYM_FUNC:
        return "a function";
    case PLUM_SYM_TREE:
        return "a tree";
    case PLUM_SYM_TRAIT:
        return "a trait";
    case PLUM_SYM_KIND:
        return "a kind";
    case PLUM_SYM_CASE:
        return "a kind case";
    case PLUM_SYM_BUILTIN_FUNC:
        return "a built-in function";
    case PLUM_SYM_BUILTIN_TYPE:
        return "a built-in type";
    }
    return "a name";
}

/* ------------------------------------------------------------------ */
/* Scopes and symbols                                                  */
/* ------------------------------------------------------------------ */

static void push(Resolver *r, Scope *scope, ScopeKind kind) {
    scope->parent = r->scope;
    scope->kind = kind;
    plum_map_init(&scope->names);
    r->scope = scope;
}

static void pop(Resolver *r) {
    Scope *scope = r->scope;
    r->scope = scope->parent;
    plum_map_free(&scope->names);
}

static PlumSymbol *new_symbol(Resolver *r, PlumSymbolKind kind, const char *name, PlumSpan span,
                              PlumStmt *decl) {
    PlumSymbol *sym = plum_arena_alloc_zeroed(r->arena, sizeof *sym);
    sym->kind = kind;
    sym->name = name;
    sym->span = span;
    sym->decl = decl;
    sym->function_depth = r->function_depth;
    sym->global = r->scope != NULL && r->scope->kind == SCOPE_MODULE;
    sym->defined = true;
    return sym;
}

static PlumSymbol *find_in(const Scope *scope, const char *name) {
    void *found = NULL;
    if (plum_map_get(&scope->names, name, strlen(name), &found)) {
        return found;
    }
    return NULL;
}

/* Declare `sym` in the current scope; a second declaration is an error. */
static void declare(Resolver *r, PlumSymbol *sym) {
    PlumSymbol *existing = find_in(r->scope, sym->name);
    if (existing != NULL) {
        plum_diag_error(r->diags, sym->span, "E0302", "'%s' is already declared on line %u, as %s",
                        sym->name, (unsigned)existing->span.line,
                        plum_symbol_kind_text(existing->kind));
        return;
    }
    plum_map_set(&r->scope->names, sym->name, strlen(sym->name), sym);
}

static PlumSymbol *lookup(const Resolver *r, const char *name) {
    for (const Scope *s = r->scope; s != NULL; s = s->parent) {
        PlumSymbol *sym = find_in(s, name);
        if (sym != NULL) {
            return sym;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* "Did you mean ...?"                                                 */
/* ------------------------------------------------------------------ */

/*
 * Edits needed to turn one name into the other, ignoring case: inserting,
 * deleting or changing a letter, or swapping two neighbours, each count
 * one (optimal string alignment distance).
 */
static size_t edit_distance(const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    if (n > 32 || m > 32) {
        return (size_t)-1;
    }
    size_t d[33][33];
    for (size_t i = 0; i <= n; i++) {
        d[i][0] = i;
    }
    for (size_t j = 0; j <= m; j++) {
        d[0][j] = j;
    }
    for (size_t i = 1; i <= n; i++) {
        for (size_t j = 1; j <= m; j++) {
            int ai = tolower((unsigned char)a[i - 1]);
            int bj = tolower((unsigned char)b[j - 1]);
            size_t best = d[i - 1][j - 1] + (ai == bj ? 0 : 1);
            if (d[i - 1][j] + 1 < best) {
                best = d[i - 1][j] + 1;
            }
            if (d[i][j - 1] + 1 < best) {
                best = d[i][j - 1] + 1;
            }
            if (i > 1 && j > 1 && ai == tolower((unsigned char)b[j - 2]) &&
                tolower((unsigned char)a[i - 2]) == bj && d[i - 2][j - 2] + 1 < best) {
                best = d[i - 2][j - 2] + 1;
            }
            d[i][j] = best;
        }
    }
    return d[n][m];
}

/* The visible name closest to `name`, if close enough to be a typo. */
static const char *suggest(const Resolver *r, const char *name, bool types_only) {
    size_t len = strlen(name);
    /* Close enough to be a typo: about a third of the letters may differ. */
    size_t limit = len <= 3 ? 1 : (len + 1) / 3;
    const char *best = NULL;
    size_t best_distance = limit + 1;
    for (const Scope *s = r->scope; s != NULL; s = s->parent) {
        size_t cursor = 0;
        const PlumMapEntry *e;
        while ((e = plum_map_next(&s->names, &cursor)) != NULL) {
            const PlumSymbol *sym = e->value;
            if (types_only && sym->kind != PLUM_SYM_BUILTIN_TYPE && sym->kind != PLUM_SYM_TREE &&
                sym->kind != PLUM_SYM_TRAIT && sym->kind != PLUM_SYM_KIND) {
                continue;
            }
            size_t d = edit_distance(name, sym->name);
            if (d < best_distance && d < len) {
                best_distance = d;
                best = sym->name;
            }
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

static bool is_type_symbol(PlumSymbolKind kind) {
    return kind == PLUM_SYM_BUILTIN_TYPE || kind == PLUM_SYM_TREE || kind == PLUM_SYM_TRAIT ||
           kind == PLUM_SYM_KIND;
}

/* Type names from other languages, with the Plumlang type. */
static const char *foreign_type(const char *name) {
    static const struct {
        const char *name;
        const char *type;
    } table[] = {
        {"boolean", "bool"},        {"Boolean", "bool"},      {"string", "str"},
        {"String", "str"},          {"char", "str"},          {"integer", "int"},
        {"Integer", "int"},         {"long", "int"},          {"double", "float"},
        {"number", "int or float"}, {"void", "none"},         {"array", "list[T]"},
        {"Array", "list[T]"},       {"List", "list[T]"},      {"dict", "map[K, V]"},
        {"Dict", "map[K, V]"},      {"HashMap", "map[K, V]"}, {"Map", "map[K, V]"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return table[i].type;
        }
    }
    return NULL;
}

static void resolve_type(Resolver *r, PlumType *type) {
    if (type == NULL) {
        return;
    }
    switch (type->kind) {
    case PLUM_TYPE_NAME: {
        const char *name = type->as.name.name;
        PlumSymbol *sym = lookup(r, name);
        if (sym == NULL) {
            const char *close = suggest(r, name, true);
            const char *theirs = foreign_type(name);
            if (theirs != NULL) {
                plum_diag_error(r->diags, type->span, "E0305",
                                "unknown type '%s'; in Plumlang it is '%s'", name, theirs);
            } else if (close != NULL) {
                plum_diag_error(r->diags, type->span, "E0305",
                                "unknown type '%s'; did you mean '%s'?", name, close);
            } else {
                plum_diag_error(r->diags, type->span, "E0305", "unknown type '%s'", name);
            }
        } else if (!is_type_symbol(sym->kind)) {
            plum_diag_error(r->diags, type->span, "E0305", "'%s' is %s, not a type", name,
                            plum_symbol_kind_text(sym->kind));
        } else {
            type->as.name.symbol = sym;
            sym->used = true;
        }
        for (size_t i = 0; i < type->as.name.arg_count; i++) {
            resolve_type(r, type->as.name.args[i]);
        }
        break;
    }
    case PLUM_TYPE_OPTIONAL:
        resolve_type(r, type->as.optional);
        break;
    case PLUM_TYPE_FUNC:
        for (size_t i = 0; i < type->as.func.param_count; i++) {
            resolve_type(r, type->as.func.params[i]);
        }
        resolve_type(r, type->as.func.result);
        break;
    case PLUM_TYPE_ERROR:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static void resolve_expr(Resolver *r, PlumExpr *e);
static void resolve_stmt(Resolver *r, PlumStmt *s);
static void resolve_body(Resolver *r, PlumStmt *block);

/* Names from other languages, with the Plumlang way to say them. */
static const char *foreign_name(const char *name) {
    static const struct {
        const char *name;
        const char *hint;
    } table[] = {
        {"print", "Plumlang prints with drop(...)"},
        {"println", "Plumlang prints with drop(...)"},
        {"printf", "Plumlang prints with drop(...); format with \"{value:.2}\""},
        {"puts", "Plumlang prints with drop(...)"},
        {"console", "Plumlang prints with drop(...)"},
        {"echo", "Plumlang prints with drop(...)"},
        {"input", "Plumlang reads a line with ask(...)"},
        {"null", "the absent value is 'none'"},
        {"nil", "the absent value is 'none'"},
        {"None", "the absent value is 'none'"},
        {"undefined", "the absent value is 'none'"},
        {"True", "true is written 'good' (or 'true')"},
        {"False", "false is written 'bad' (or 'false')"},
        {"this", "the current object is 'self'"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return table[i].hint;
        }
    }
    return NULL;
}

static void resolve_name(Resolver *r, PlumExpr *e) {
    const char *name = e->as.name;
    PlumSymbol *sym = lookup(r, name);
    if (sym == NULL && foreign_name(name) != NULL) {
        plum_diag_error(r->diags, e->span, "E0301", "unknown name '%s'; %s", name,
                        foreign_name(name));
        return;
    }
    if (sym == NULL) {
        const char *close = suggest(r, name, false);
        if (close != NULL) {
            plum_diag_error(r->diags, e->span, "E0301", "unknown name '%s'; did you mean '%s'?",
                            name, close);
        } else {
            plum_diag_error(r->diags, e->span, "E0301", "unknown name '%s'", name);
        }
        return;
    }
    if (sym->global && !sym->defined && r->function_depth == 0) {
        plum_diag_error(r->diags, e->span, "E0303",
                        "'%s' is used before its declaration on line %u; move the declaration "
                        "up",
                        name, (unsigned)sym->span.line);
    }
    if (!sym->global && sym->function_depth < r->function_depth &&
        (sym->kind == PLUM_SYM_VAR || sym->kind == PLUM_SYM_CONST || sym->kind == PLUM_SYM_PARAM ||
         sym->kind == PLUM_SYM_LOOP || sym->kind == PLUM_SYM_BINDING ||
         sym->kind == PLUM_SYM_FUNC)) { /* a nested function calling itself, too */
        sym->captured = true;           /* a closure uses a local of an outer function */
    }
    sym->used = true;
    e->symbol = sym;
}

static void resolve_params(Resolver *r, PlumParam *params, size_t count) {
    for (size_t i = 0; i < count; i++) {
        resolve_type(r, params[i].type);
        if (params[i].fallback != NULL) {
            resolve_expr(r, params[i].fallback);
        }
    }
    for (size_t i = 0; i < count; i++) {
        PlumSymbol *sym = new_symbol(r, PLUM_SYM_PARAM, params[i].name, params[i].span, NULL);
        declare(r, sym);
        params[i].symbol = sym;
    }
}

static void resolve_lambda(Resolver *r, PlumExpr *e) {
    Scope scope;
    r->function_depth++;
    push(r, &scope, SCOPE_FUNCTION);
    resolve_params(r, e->as.lambda.params, e->as.lambda.param_count);
    if (e->as.lambda.block != NULL) {
        resolve_body(r, e->as.lambda.block);
    } else {
        resolve_expr(r, e->as.lambda.body);
    }
    pop(r);
    r->function_depth--;
}

static void pattern_error(Resolver *r, const PlumExpr *e) {
    plum_diag_error(r->diags, e->span, "E0309",
                    "a pattern must be a literal, a range, a kind case or '_'");
}

/* A name bound by a pattern: `Circle(r) =>` binds r. */
static void bind_pattern_name(Resolver *r, PlumExpr *e) {
    if (e->kind == PLUM_EXPR_WILDCARD) {
        return;
    }
    if (e->kind != PLUM_EXPR_NAME) {
        plum_diag_error(r->diags, e->span, "E0309",
                        "inside a case pattern, write a name to bind its value, or '_'");
        return;
    }
    PlumSymbol *sym = new_symbol(r, PLUM_SYM_BINDING, e->as.name, e->span, NULL);
    declare(r, sym);
    e->symbol = sym;
}

static PlumSymbol *find_case(const Resolver *r, const char *name) {
    void *found = NULL;
    if (plum_map_get(&r->cases, name, strlen(name), &found)) {
        return found;
    }
    return NULL;
}

static void resolve_pattern(Resolver *r, PlumExpr *e) {
    switch (e->kind) {
    case PLUM_EXPR_WILDCARD:
    case PLUM_EXPR_INT:
    case PLUM_EXPR_FLOAT:
    case PLUM_EXPR_STRING:
    case PLUM_EXPR_BOOL:
    case PLUM_EXPR_NONE:
        return;
    case PLUM_EXPR_UNARY:
    case PLUM_EXPR_RANGE:
    case PLUM_EXPR_FIELD: /* Direction.North */
        resolve_expr(r, e);
        return;
    case PLUM_EXPR_NAME: {
        PlumSymbol *c = find_case(r, e->as.name);
        if (c != NULL) {
            e->symbol = c;
            c->used = true;
            return;
        }
        PlumSymbol *sym = lookup(r, e->as.name);
        if (sym != NULL && sym->kind == PLUM_SYM_CONST) {
            resolve_name(r, e); /* a root constant matches by value */
            return;
        }
        plum_diag_error(r->diags, e->span, "E0309",
                        "'%s' is not a kind case or a root constant; a pattern can't bind a "
                        "bare name (use 'else' to match anything)",
                        e->as.name);
        return;
    }
    case PLUM_EXPR_CALL: {
        PlumExpr *callee = e->as.call.callee;
        PlumSymbol *c = callee->kind == PLUM_EXPR_NAME ? find_case(r, callee->as.name) : NULL;
        if (c == NULL) {
            pattern_error(r, e);
            return;
        }
        callee->symbol = c;
        c->used = true;
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            bind_pattern_name(r, e->as.call.args[i].value);
        }
        return;
    }
    default:
        pattern_error(r, e);
        return;
    }
}

static void resolve_match(Resolver *r, PlumExpr *e) {
    resolve_expr(r, e->as.match.subject);
    for (size_t i = 0; i < e->as.match.arm_count; i++) {
        PlumMatchArm *arm = &e->as.match.arms[i];
        Scope scope;
        push(r, &scope, SCOPE_BLOCK);
        for (size_t k = 0; k < arm->pattern_count; k++) {
            resolve_pattern(r, arm->patterns[k]);
        }
        resolve_stmt(r, arm->body);
        pop(r);
    }
}

static void resolve_expr(Resolver *r, PlumExpr *e) {
    if (e == NULL) {
        return;
    }
    switch (e->kind) {
    case PLUM_EXPR_INT:
    case PLUM_EXPR_FLOAT:
    case PLUM_EXPR_STRING:
    case PLUM_EXPR_BOOL:
    case PLUM_EXPR_NONE:
    case PLUM_EXPR_ERROR:
        break;
    case PLUM_EXPR_NAME:
        resolve_name(r, e);
        break;
    case PLUM_EXPR_SELF:
    case PLUM_EXPR_SUPER:
        if (!r->in_method) {
            plum_diag_error(r->diags, e->span, "E0306", "'%s' can only be used inside a method",
                            e->kind == PLUM_EXPR_SELF ? "self" : "super");
        }
        break;
    case PLUM_EXPR_INTERP:
        for (size_t i = 0; i < e->as.interp.count; i++) {
            resolve_expr(r, e->as.interp.parts[i].expr);
        }
        break;
    case PLUM_EXPR_LIST:
        for (size_t i = 0; i < e->as.list.count; i++) {
            resolve_expr(r, e->as.list.items[i]);
        }
        break;
    case PLUM_EXPR_MAP:
        for (size_t i = 0; i < e->as.map.count; i++) {
            resolve_expr(r, e->as.map.entries[i].key);
            resolve_expr(r, e->as.map.entries[i].value);
        }
        break;
    case PLUM_EXPR_UNARY:
        resolve_expr(r, e->as.unary.operand);
        break;
    case PLUM_EXPR_BINARY:
        resolve_expr(r, e->as.binary.left);
        resolve_expr(r, e->as.binary.right);
        break;
    case PLUM_EXPR_RANGE:
        resolve_expr(r, e->as.range.low);
        resolve_expr(r, e->as.range.high);
        break;
    case PLUM_EXPR_IS:
    case PLUM_EXPR_AS:
        resolve_expr(r, e->as.cast.value);
        resolve_type(r, e->as.cast.type);
        break;
    case PLUM_EXPR_CALL:
        resolve_expr(r, e->as.call.callee);
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            resolve_expr(r, e->as.call.args[i].value);
        }
        break;
    case PLUM_EXPR_INDEX:
        resolve_expr(r, e->as.index.object);
        resolve_expr(r, e->as.index.index);
        break;
    case PLUM_EXPR_FIELD:
        resolve_expr(r, e->as.field.object); /* the field itself is checked with types */
        break;
    case PLUM_EXPR_LAMBDA:
        resolve_lambda(r, e);
        break;
    case PLUM_EXPR_IF:
        resolve_expr(r, e->as.if_.cond);
        resolve_expr(r, e->as.if_.then);
        resolve_expr(r, e->as.if_.otherwise);
        break;
    case PLUM_EXPR_TRY:
    case PLUM_EXPR_SPAWN:
        resolve_expr(r, e->as.inner);
        break;
    case PLUM_EXPR_MATCH:
        resolve_match(r, e);
        break;
    case PLUM_EXPR_WILDCARD:
        plum_diag_error(r->diags, e->span, "E0309",
                        "'_' can only be used in a pattern or as a for loop name");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

/* Report an assignment to something that can't change. */
static void check_assignable(Resolver *r, const PlumExpr *target) {
    if (target->kind != PLUM_EXPR_NAME || target->symbol == NULL) {
        return;
    }
    const PlumSymbol *sym = target->symbol;
    const char *name = sym->name;
    switch (sym->kind) {
    case PLUM_SYM_VAR:
        return;
    case PLUM_SYM_CONST:
        plum_diag_error(r->diags, target->span, "E0304",
                        "can't change '%s': it is a root constant; declare it with 'plant' to "
                        "change it",
                        name);
        return;
    case PLUM_SYM_PARAM:
        plum_diag_error(r->diags, target->span, "E0304",
                        "can't change the parameter '%s'; copy it first: plant %s2 = %s", name,
                        name, name);
        return;
    case PLUM_SYM_LOOP:
        plum_diag_error(r->diags, target->span, "E0304",
                        "can't change the loop variable '%s'; it is set by the loop", name);
        return;
    default:
        plum_diag_error(r->diags, target->span, "E0304", "can't change '%s': it is %s", name,
                        plum_symbol_kind_text(sym->kind));
        return;
    }
}

static void resolve_function(Resolver *r, PlumBranch *b, bool method) {
    Scope scope;
    bool was_method = r->in_method;
    r->in_method = method;
    r->function_depth++;
    push(r, &scope, SCOPE_FUNCTION);
    resolve_type(r, b->result);
    resolve_params(r, b->params, b->param_count);
    if (b->arrow != NULL) {
        resolve_expr(r, b->arrow);
    } else if (b->body != NULL) {
        resolve_body(r, b->body);
    }
    pop(r);
    r->function_depth--;
    r->in_method = was_method;
}

/* A function's body shares the parameters' scope, so `plant a` can't
 * quietly hide a parameter `a`. */
static void resolve_body(Resolver *r, PlumStmt *block) {
    if (block->kind != PLUM_STMT_BLOCK) {
        resolve_stmt(r, block);
        return;
    }
    for (size_t i = 0; i < block->as.block.count; i++) {
        resolve_stmt(r, block->as.block.stmts[i]);
    }
}

static void resolve_block(Resolver *r, PlumStmt *block) {
    if (block == NULL) {
        return;
    }
    Scope scope;
    push(r, &scope, SCOPE_BLOCK);
    resolve_body(r, block);
    pop(r);
}

static void resolve_tree(Resolver *r, PlumStmt *s) {
    if (r->function_depth > 0) {
        plum_diag_error(r->diags, s->span, "E0310", "%s can only be declared at the top of a file",
                        s->kind == PLUM_STMT_TREE ? "a tree" : "a trait");
        return;
    }
    for (size_t i = 0; i < s->as.tree.super_count; i++) {
        resolve_type(r, s->as.tree.supers[i]);
    }
    for (size_t i = 0; i < s->as.tree.field_count; i++) {
        PlumField *f = &s->as.tree.fields[i];
        resolve_type(r, f->type);
        resolve_expr(r, f->fallback);
        for (size_t k = 0; k < i; k++) {
            if (s->as.tree.fields[k].name == f->name) {
                plum_diag_error(r->diags, f->span, "E0302",
                                "the field '%s' is already declared on line %u", f->name,
                                (unsigned)s->as.tree.fields[k].span.line);
            }
        }
    }
    for (size_t i = 0; i < s->as.tree.method_count; i++) {
        resolve_function(r, &s->as.tree.methods[i], true);
    }
}

static void resolve_stmt(Resolver *r, PlumStmt *s) {
    switch (s->kind) {
    case PLUM_STMT_EXPR:
    case PLUM_STMT_THROW:
        resolve_expr(r, s->as.expr);
        break;
    case PLUM_STMT_RETURN:
        if (r->function_depth == 0) {
            plum_diag_error(r->diags, s->span, "E0307",
                            "'return' can only be used inside a function");
        }
        resolve_expr(r, s->as.expr);
        break;
    case PLUM_STMT_DECL: {
        resolve_type(r, s->as.decl.type);
        resolve_expr(r, s->as.decl.value); /* before the name exists: plant x = x + 1 */
        if (s->symbol != NULL) {
            s->symbol->defined = true; /* declared ahead at module level */
        } else {
            PlumSymbol *sym = new_symbol(r, s->as.decl.constant ? PLUM_SYM_CONST : PLUM_SYM_VAR,
                                         s->as.decl.name, s->span, s);
            declare(r, sym);
            s->symbol = sym;
        }
        break;
    }
    case PLUM_STMT_ASSIGN:
        resolve_expr(r, s->as.assign.value);
        resolve_expr(r, s->as.assign.target);
        check_assignable(r, s->as.assign.target);
        break;
    case PLUM_STMT_BLOCK:
        resolve_block(r, s);
        break;
    case PLUM_STMT_IF:
        resolve_expr(r, s->as.if_.cond);
        resolve_block(r, s->as.if_.then);
        if (s->as.if_.otherwise != NULL) {
            resolve_stmt(r, s->as.if_.otherwise);
        }
        break;
    case PLUM_STMT_IF_PLANT: {
        resolve_expr(r, s->as.if_.value);
        Scope scope;
        push(r, &scope, SCOPE_BLOCK);
        PlumSymbol *sym = new_symbol(r, PLUM_SYM_BINDING, s->as.if_.name, s->span, s);
        declare(r, sym);
        s->symbol = sym;
        resolve_block(r, s->as.if_.then);
        pop(r);
        if (s->as.if_.otherwise != NULL) {
            resolve_stmt(r, s->as.if_.otherwise);
        }
        break;
    }
    case PLUM_STMT_WHILE:
        resolve_expr(r, s->as.while_.cond);
        resolve_block(r, s->as.while_.body);
        break;
    case PLUM_STMT_FOR: {
        resolve_expr(r, s->as.for_.iterable);
        Scope scope;
        push(r, &scope, SCOPE_BLOCK);
        const char *names[2] = {s->as.for_.first, s->as.for_.second};
        PlumSymbol **slots[2] = {&s->symbol, &s->symbol2};
        for (int i = 0; i < 2; i++) {
            if (names[i] != NULL && strcmp(names[i], "_") != 0) {
                PlumSymbol *sym = new_symbol(r, PLUM_SYM_LOOP, names[i], s->span, s);
                declare(r, sym);
                *slots[i] = sym;
            }
        }
        resolve_block(r, s->as.for_.body);
        pop(r);
        break;
    }
    case PLUM_STMT_BREAK:
    case PLUM_STMT_CONTINUE:
    case PLUM_STMT_ERROR:
        break;
    case PLUM_STMT_TRY:
        resolve_block(r, s->as.try_.body);
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            PlumCatch *c = &s->as.try_.catches[i];
            resolve_type(r, c->type);
            Scope scope;
            push(r, &scope, SCOPE_BLOCK);
            c->symbol = new_symbol(r, PLUM_SYM_BINDING, c->name, c->span, s);
            declare(r, c->symbol);
            resolve_block(r, c->body);
            pop(r);
        }
        resolve_block(r, s->as.try_.finally);
        break;
    case PLUM_STMT_BRANCH:
        if (s->symbol == NULL) {
            /* A function inside a function: visible from here on, and to
             * itself, so it can recurse. */
            PlumSymbol *sym = new_symbol(r, PLUM_SYM_FUNC, s->as.branch.name, s->span, s);
            declare(r, sym);
            s->symbol = sym;
        }
        resolve_function(r, &s->as.branch, false);
        break;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
        resolve_tree(r, s);
        break;
    case PLUM_STMT_KIND:
        if (r->function_depth > 0) {
            plum_diag_error(r->diags, s->span, "E0310",
                            "a kind can only be declared at the top of a file");
            break;
        }
        for (size_t i = 0; i < s->as.kind.case_count; i++) {
            PlumCase *c = &s->as.kind.cases[i];
            for (size_t k = 0; k < c->param_count; k++) {
                resolve_type(r, c->params[k].type);
            }
        }
        break;
    case PLUM_STMT_SEED:
        plum_diag_error(r->diags, s->span, "E0308",
                        "importing modules is not supported yet; it arrives with #23");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Built-ins and the module                                            */
/* ------------------------------------------------------------------ */

static void add_builtin(Resolver *r, PlumSymbolKind kind, const char *name) {
    PlumSpan none = {0, 0, 0, 0};
    PlumSymbol *sym = new_symbol(r, kind, plum_intern_cstr(r->names, name), none, NULL);
    sym->global = false;
    plum_map_set(&r->scope->names, sym->name, strlen(sym->name), sym);
}

static void add_builtins(Resolver *r) {
    static const char *const types[] = {"int",  "float", "bool", "str",  "byte",   "none",
                                        "list", "map",   "set",  "Task", "Channel"};
    static const char *const functions[] = {"drop", "ask", "len", "type_of", "assert", "exit"};
    static const char *const errors[] = {
        "Error",         "ValueError",        "ParseError", "IndexError", "KeyError",
        "OverflowError", "ZeroDivisionError", "IOError",    "NetError",   "AssertError"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        add_builtin(r, PLUM_SYM_BUILTIN_TYPE, types[i]);
    }
    for (size_t i = 0; i < sizeof functions / sizeof functions[0]; i++) {
        add_builtin(r, PLUM_SYM_BUILTIN_FUNC, functions[i]);
    }
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
        add_builtin(r, PLUM_SYM_TREE, errors[i]);
    }
}

/*
 * Declare every module-level name before resolving anything, so functions,
 * trees, traits and kinds can be used anywhere in the file, and variables
 * from inside function bodies wherever they are declared.
 */
static void declare_module(Resolver *r, const PlumProgram *program) {
    for (size_t i = 0; i < program->count; i++) {
        PlumStmt *s = program->stmts[i];
        PlumSymbol *sym = NULL;
        switch (s->kind) {
        case PLUM_STMT_DECL:
            sym = new_symbol(r, s->as.decl.constant ? PLUM_SYM_CONST : PLUM_SYM_VAR,
                             s->as.decl.name, s->span, s);
            sym->defined = false; /* until its line is reached */
            break;
        case PLUM_STMT_BRANCH:
            sym = new_symbol(r, PLUM_SYM_FUNC, s->as.branch.name, s->span, s);
            break;
        case PLUM_STMT_TREE:
            sym = new_symbol(r, PLUM_SYM_TREE, s->as.tree.name, s->span, s);
            break;
        case PLUM_STMT_TRAIT:
            sym = new_symbol(r, PLUM_SYM_TRAIT, s->as.tree.name, s->span, s);
            break;
        case PLUM_STMT_KIND:
            sym = new_symbol(r, PLUM_SYM_KIND, s->as.kind.name, s->span, s);
            for (size_t k = 0; k < s->as.kind.case_count; k++) {
                PlumCase *c = &s->as.kind.cases[k];
                PlumSymbol *cs = new_symbol(r, PLUM_SYM_CASE, c->name, c->span, s);
                cs->owner = sym;
                void *other = NULL;
                if (plum_map_get(&r->cases, c->name, strlen(c->name), &other)) {
                    const PlumSymbol *prev = other;
                    plum_diag_error(r->diags, c->span, "E0302",
                                    "the case '%s' is already declared on line %u; cases of "
                                    "different kinds need different names",
                                    c->name, (unsigned)prev->span.line);
                } else {
                    plum_map_set(&r->cases, c->name, strlen(c->name), cs);
                }
            }
            break;
        default:
            break;
        }
        if (sym != NULL) {
            declare(r, sym);
            s->symbol = sym;
        }
    }
}

void plum_resolve(const PlumProgram *program, PlumArena *arena, PlumInterner *names,
                  PlumDiagList *diags) {
    Resolver r;
    memset(&r, 0, sizeof r);
    r.arena = arena;
    r.names = names;
    r.diags = diags;
    plum_map_init(&r.cases);

    Scope builtins, module;
    push(&r, &builtins, SCOPE_BUILTIN);
    add_builtins(&r);
    push(&r, &module, SCOPE_MODULE);

    declare_module(&r, program);
    for (size_t i = 0; i < program->count; i++) {
        resolve_stmt(&r, program->stmts[i]);
    }

    pop(&r);
    pop(&r);
    plum_map_free(&r.cases);
}
