/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The syntax tree. Every node lives in the compile arena and carries the
 * span of source it came from. Child lists are arena arrays with a count.
 */
#ifndef PLUM_PARSER_AST_H
#define PLUM_PARSER_AST_H

#include "diag/diag.h"
#include "lexer/token.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct PlumExpr PlumExpr;
typedef struct PlumStmt PlumStmt;
typedef struct PlumSymbol PlumSymbol; /* filled in by the resolver (src/check/) */
typedef struct PlumTy PlumTy;         /* filled in by the type checker */
typedef struct PlumType PlumType;

/* ------------------------------------------------------------------ */
/* Types written in source: int, list[str], str?, (int) -> bool        */
/* ------------------------------------------------------------------ */

typedef enum PlumTypeKind {
    PLUM_TYPE_NAME,     /* int, Account, list[int], map[str, int] */
    PLUM_TYPE_OPTIONAL, /* T? */
    PLUM_TYPE_FUNC,     /* (A, B) -> R */
    PLUM_TYPE_ERROR,
} PlumTypeKind;

struct PlumType {
    PlumTypeKind kind;
    PlumSpan span;
    union {
        struct {
            const char *name; /* interned */
            PlumType **args;  /* generic arguments: list[T] */
            size_t arg_count;
            PlumSymbol *symbol; /* the type it names, after resolution */
        } name;
        PlumType *optional; /* the T of T? */
        struct {
            PlumType **params;
            size_t param_count;
            PlumType *result; /* NULL: returns none */
        } func;
    } as;
};

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

typedef enum PlumExprKind {
    PLUM_EXPR_INT,
    PLUM_EXPR_FLOAT,
    PLUM_EXPR_STRING,
    PLUM_EXPR_INTERP, /* "a{x}b{y:.2}" */
    PLUM_EXPR_BOOL,
    PLUM_EXPR_NONE,
    PLUM_EXPR_NAME,
    PLUM_EXPR_SELF,
    PLUM_EXPR_SUPER,
    PLUM_EXPR_LIST,
    PLUM_EXPR_MAP,
    PLUM_EXPR_UNARY,  /* - ~ not */
    PLUM_EXPR_BINARY, /* arithmetic, comparison, and/or, ??, in */
    PLUM_EXPR_RANGE,  /* a..b, a..=b */
    PLUM_EXPR_IS,     /* x is Type */
    PLUM_EXPR_AS,     /* x as Type */
    PLUM_EXPR_CALL,
    PLUM_EXPR_INDEX, /* a[i], a[1..3] */
    PLUM_EXPR_FIELD, /* a.b, a?.b */
    PLUM_EXPR_LAMBDA,
    PLUM_EXPR_IF,       /* if c { a } else { b } */
    PLUM_EXPR_TRY,      /* try expr */
    PLUM_EXPR_SPAWN,    /* spawn call */
    PLUM_EXPR_MATCH,    /* match x { pattern => ... } */
    PLUM_EXPR_WILDCARD, /* _ in a pattern */
    PLUM_EXPR_ERROR,    /* stands in for an expression that failed to parse */
} PlumExprKind;

/* One piece of an interpolated string: text, or an expression and format. */
typedef struct PlumInterpPart {
    const char *text; /* the literal text, if expr is NULL */
    size_t len;
    PlumExpr *expr;
    const char *format; /* "{x:.2}" -> ".2"; NULL when none */
} PlumInterpPart;

typedef struct PlumArg {
    const char *name; /* named argument, or NULL */
    PlumExpr *value;
    PlumSpan span;
} PlumArg;

typedef struct PlumMapEntryExpr {
    PlumExpr *key;
    PlumExpr *value;
} PlumMapEntryExpr;

/* One arm of a match: `a, b => body`, or `else => body`. */
typedef struct PlumMatchArm {
    PlumExpr **patterns; /* none for else */
    size_t pattern_count;
    PlumStmt *body; /* an expression statement or a block */
    PlumSpan span;
} PlumMatchArm;

typedef struct PlumParam {
    const char *name;
    PlumType *type;     /* NULL when left out (lambdas only) */
    PlumExpr *fallback; /* default value, or NULL */
    PlumSpan span;
    PlumSymbol *symbol; /* after resolution */
} PlumParam;

struct PlumExpr {
    PlumExprKind kind;
    PlumSpan span;
    PlumSymbol *symbol; /* NAME: what it refers to, after resolution */
    PlumTy *ty;         /* its type, after type checking */
    union {
        int64_t i;
        double f;
        bool b;
        struct {
            const char *chars;
            size_t len;
        } str;
        const char *name; /* NAME: interned */
        struct {
            PlumInterpPart *parts;
            size_t count;
        } interp;
        struct {
            PlumExpr **items;
            size_t count;
        } list;
        struct {
            PlumMapEntryExpr *entries;
            size_t count;
        } map;
        struct {
            PlumTokenKind op;
            PlumExpr *operand;
        } unary;
        struct {
            PlumTokenKind op;
            PlumExpr *left;
            PlumExpr *right;
        } binary;
        struct {
            PlumExpr *low;
            PlumExpr *high;
            bool inclusive;
        } range;
        struct {
            PlumExpr *value;
            PlumType *type;
        } cast; /* IS and AS */
        struct {
            PlumExpr *callee;
            PlumArg *args;
            size_t arg_count;
        } call;
        struct {
            PlumExpr *object;
            PlumExpr *index;
        } index;
        struct {
            PlumExpr *object;
            const char *name;
            bool optional; /* ?. */
        } field;
        struct {
            PlumParam *params;
            size_t param_count;
            PlumExpr *body;  /* (x) => expr */
            PlumStmt *block; /* (x) => { ... }; body is NULL then */
        } lambda;
        struct {
            PlumExpr *cond;
            PlumExpr *then;
            PlumExpr *otherwise; /* NULL without else */
        } if_;
        PlumExpr *inner; /* TRY, SPAWN */
        struct {
            PlumExpr *subject;
            PlumMatchArm *arms;
            size_t arm_count;
        } match;
    } as;
};

/* ------------------------------------------------------------------ */
/* Statements and declarations                                         */
/* ------------------------------------------------------------------ */

typedef enum PlumStmtKind {
    PLUM_STMT_EXPR,     /* a call standing alone */
    PLUM_STMT_DECL,     /* plant / root */
    PLUM_STMT_ASSIGN,   /* = += -= ... */
    PLUM_STMT_BLOCK,    /* { ... } */
    PLUM_STMT_IF,       /* if cond { } else { } */
    PLUM_STMT_IF_PLANT, /* if plant x = maybe { } else { } */
    PLUM_STMT_WHILE,
    PLUM_STMT_FOR,
    PLUM_STMT_BREAK,
    PLUM_STMT_CONTINUE,
    PLUM_STMT_RETURN,
    PLUM_STMT_THROW,
    PLUM_STMT_TRY,
    PLUM_STMT_BRANCH, /* function or method */
    PLUM_STMT_TREE,
    PLUM_STMT_TRAIT,
    PLUM_STMT_KIND,
    PLUM_STMT_SEED,
    PLUM_STMT_ERROR,
} PlumStmtKind;

typedef struct PlumCatch {
    const char *name; /* the error variable */
    PlumType *type;   /* NULL: catches every error */
    PlumStmt *body;
    PlumSpan span;
    PlumSymbol *symbol; /* after resolution */
    PlumSymbol *error;  /* after resolution: the built-in Error, the type of a plain catch */
} PlumCatch;

typedef struct PlumField {
    const char *name;
    PlumType *type;
    PlumExpr *fallback; /* default value, or NULL */
    bool weak;
    PlumSpan span;
} PlumField;

/* A kind case: `North`, or `Circle(radius: float)`. */
typedef struct PlumCase {
    const char *name;
    PlumParam *params;
    size_t param_count;
    PlumSpan span;
} PlumCase;

typedef struct PlumBranch {
    const char *name;
    PlumParam *params;
    size_t param_count;
    PlumType *result; /* NULL: returns none */
    PlumStmt *body;   /* a block; NULL for a trait method without a default */
    PlumExpr *arrow;  /* `=> expr` bodies; body is NULL then */
} PlumBranch;

struct PlumStmt {
    PlumStmtKind kind;
    PlumSpan span;
    /* After resolution: what DECL, IF_PLANT, BRANCH, TREE, TRAIT and KIND
     * declare, and FOR's first name; symbol2 is FOR's second name. */
    PlumSymbol *symbol;
    PlumSymbol *symbol2;
    union {
        PlumExpr *expr; /* EXPR, THROW; RETURN (NULL: bare return) */
        struct {
            bool constant; /* root */
            const char *name;
            PlumType *type; /* NULL: inferred */
            PlumExpr *value;
        } decl;
        struct {
            PlumTokenKind op; /* EQ or a compound operator */
            PlumExpr *target;
            PlumExpr *value;
        } assign;
        struct {
            PlumStmt **stmts;
            size_t count;
        } block;
        struct {
            PlumExpr *cond;   /* IF */
            const char *name; /* IF_PLANT: the unwrapped name */
            PlumExpr *value;  /* IF_PLANT: the optional value */
            PlumStmt *then;
            PlumStmt *otherwise; /* a block, another IF, or NULL */
        } if_;
        struct {
            PlumExpr *cond;
            PlumStmt *body;
        } while_;
        struct {
            const char *first;  /* `for x in` / `for i, x in` */
            const char *second; /* NULL with one name */
            PlumExpr *iterable;
            PlumStmt *body;
        } for_;
        struct {
            PlumStmt *body;
            PlumCatch *catches;
            size_t catch_count;
            PlumStmt *finally; /* NULL without finally */
        } try_;
        PlumBranch branch;
        struct {
            const char *name;
            PlumType **supers; /* parent tree and traits after ':' */
            size_t super_count;
            PlumField *fields;
            size_t field_count;
            PlumBranch *methods;
            size_t method_count;
        } tree; /* TREE and TRAIT (traits have no fields) */
        struct {
            const char *name;
            PlumCase *cases;
            size_t case_count;
        } kind;
        struct {
            const char *path;   /* "math", "net.http", "./models/user" */
            const char *alias;  /* `as name`, or NULL */
            const char **names; /* `{ sqrt, PI }` */
            size_t name_count;
        } seed;
    } as;
};

/* A parsed file: its statements in order. */
typedef struct PlumProgram {
    PlumStmt **stmts;
    size_t count;
} PlumProgram;

/* Print an expression or type as an S-expression, for `plum ast`. */
void plum_print_expr(FILE *out, const PlumExpr *expr);
/* Print a program, one statement per line, with blocks indented. */
void plum_print_program(FILE *out, const PlumProgram *program);
void plum_print_type(FILE *out, const PlumType *type);

#endif /* PLUM_PARSER_AST_H */
