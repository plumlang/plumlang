/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Tokens: the words and symbols of a Plumlang program (spec section 2). */
#ifndef PLUM_LEXER_TOKEN_H
#define PLUM_LEXER_TOKEN_H

#include "diag/diag.h"

#include <stdint.h>

/* X(kind, text shown by `plum tokens` and in error messages) */
#define PLUM_TOKEN_KINDS(X)                              \
    /* Literals and names */                             \
    X(INT, "integer")                                    \
    X(FLOAT, "float")                                    \
    X(STRING, "string")                                  \
    X(STRING_HEAD, "string start") /* "text{   */        \
    X(STRING_MID, "string middle") /* }text{   */        \
    X(STRING_TAIL, "string end")   /* }text"   */        \
    X(FORMAT, "format")            /* :.2 in "{x:.2}" */ \
    X(IDENT, "name")                                     \
    /* Keywords */                                       \
    X(PLANT, "plant")                                    \
    X(ROOT, "root")                                      \
    X(BRANCH, "branch")                                  \
    X(TREE, "tree")                                      \
    X(TRAIT, "trait")                                    \
    X(KIND, "kind")                                      \
    X(SEED, "seed")                                      \
    X(GOOD, "good")                                      \
    X(BAD, "bad")                                        \
    X(NONE, "none")                                      \
    X(IF, "if")                                          \
    X(ELSE, "else")                                      \
    X(MATCH, "match")                                    \
    X(FOR, "for")                                        \
    X(IN, "in")                                          \
    X(WHILE, "while")                                    \
    X(BREAK, "break")                                    \
    X(CONTINUE, "continue")                              \
    X(RETURN, "return")                                  \
    X(AND, "and")                                        \
    X(OR, "or")                                          \
    X(NOT, "not")                                        \
    X(IS, "is")                                          \
    X(AS, "as")                                          \
    X(SELF, "self")                                      \
    X(SUPER, "super")                                    \
    X(THROW, "throw")                                    \
    X(TRY, "try")                                        \
    X(CATCH, "catch")                                    \
    X(FINALLY, "finally")                                \
    X(SPAWN, "spawn")                                    \
    X(WEAK, "weak")                                      \
    /* Brackets and punctuation */                       \
    X(LPAREN, "(")                                       \
    X(RPAREN, ")")                                       \
    X(LBRACKET, "[")                                     \
    X(RBRACKET, "]")                                     \
    X(LBRACE, "{")                                       \
    X(RBRACE, "}")                                       \
    X(COMMA, ",")                                        \
    X(COLON, ":")                                        \
    X(DOT, ".")                                          \
    X(DOT_DOT, "..")                                     \
    X(DOT_DOT_EQ, "..=")                                 \
    X(ARROW, "->")                                       \
    X(FAT_ARROW, "=>")                                   \
    X(QUESTION, "?")                                     \
    X(QUESTION_DOT, "?.")                                \
    X(QUESTION_QUESTION, "??")                           \
    X(UNDERSCORE, "_")                                   \
    /* Operators */                                      \
    X(PLUS, "+")                                         \
    X(MINUS, "-")                                        \
    X(STAR, "*")                                         \
    X(SLASH, "/")                                        \
    X(SLASH_SLASH, "//")                                 \
    X(PERCENT, "%")                                      \
    X(STAR_STAR, "**")                                   \
    X(AMP, "&")                                          \
    X(PIPE, "|")                                         \
    X(CARET, "^")                                        \
    X(TILDE, "~")                                        \
    X(LESS_LESS, "<<")                                   \
    X(GREATER_GREATER, ">>")                             \
    X(EQ_EQ, "==")                                       \
    X(BANG_EQ, "!=")                                     \
    X(LESS, "<")                                         \
    X(LESS_EQ, "<=")                                     \
    X(GREATER, ">")                                      \
    X(GREATER_EQ, ">=")                                  \
    /* Assignment */                                     \
    X(EQ, "=")                                           \
    X(PLUS_EQ, "+=")                                     \
    X(MINUS_EQ, "-=")                                    \
    X(STAR_EQ, "*=")                                     \
    X(SLASH_EQ, "/=")                                    \
    X(SLASH_SLASH_EQ, "//=")                             \
    X(PERCENT_EQ, "%=")                                  \
    X(STAR_STAR_EQ, "**=")                               \
    X(AMP_EQ, "&=")                                      \
    X(PIPE_EQ, "|=")                                     \
    X(CARET_EQ, "^=")                                    \
    X(LESS_LESS_EQ, "<<=")                               \
    X(GREATER_GREATER_EQ, ">>=")                         \
    /* Structure */                                      \
    X(NEWLINE, "end of line")                            \
    X(EOF, "end of file")                                \
    X(ERROR, "invalid token")

typedef enum PlumTokenKind {
#define PLUM_TOKEN_ENUM(kind, text) PLUM_TOK_##kind,
    PLUM_TOKEN_KINDS(PLUM_TOKEN_ENUM)
#undef PLUM_TOKEN_ENUM
        PLUM_TOKEN_KIND_COUNT
} PlumTokenKind;

typedef struct PlumToken {
    PlumTokenKind kind;
    PlumSpan span;
    union {
        int64_t i; /* INT */
        double f;  /* FLOAT */
        struct {   /* STRING*, FORMAT: decoded text, in the arena */
            const char *chars;
            size_t len;
        } str;
        const char *name; /* IDENT: interned */
    } as;
} PlumToken;

/* The text of a token kind: "plant", "+", "end of line". */
const char *plum_token_kind_text(PlumTokenKind kind);
/* The enum name of a token kind: "PLANT", "PLUS". */
const char *plum_token_kind_name(PlumTokenKind kind);

#endif /* PLUM_LEXER_TOKEN_H */
