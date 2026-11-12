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
    PLUM_EXPR_IF,    /* if c { a } else { b } */
    PLUM_EXPR_TRY,   /* try expr */
    PLUM_EXPR_SPAWN, /* spawn call */
    PLUM_EXPR_ERROR, /* stands in for an expression that failed to parse */
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

typedef struct PlumParam {
    const char *name;
    PlumType *type;     /* NULL when left out (lambdas only) */
    PlumExpr *fallback; /* default value, or NULL */
    PlumSpan span;
} PlumParam;

struct PlumExpr {
    PlumExprKind kind;
    PlumSpan span;
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
            PlumExpr *body;
        } lambda;
        struct {
            PlumExpr *cond;
            PlumExpr *then;
            PlumExpr *otherwise; /* NULL without else */
        } if_;
        PlumExpr *inner; /* TRY, SPAWN */
    } as;
};

/* Print an expression or type as an S-expression, for `plum ast`. */
void plum_print_expr(FILE *out, const PlumExpr *expr);
void plum_print_type(FILE *out, const PlumType *type);

#endif /* PLUM_PARSER_AST_H */
