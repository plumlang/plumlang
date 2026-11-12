/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "parser/parser.h"

#include "util/alloc.h"
#include "util/array.h"

#include <stdarg.h>
#include <string.h>

/* Binding power of each precedence level (spec 4.8), lowest first. */
enum {
    PREC_NONE = 0,
    PREC_COALESCE, /* ?? */
    PREC_OR,       /* or */
    PREC_AND,      /* and */
    PREC_NOT,      /* not (prefix) */
    PREC_COMPARE,  /* == != < <= > >= is in */
    PREC_RANGE,    /* .. ..= */
    PREC_BIT_OR,   /* | */
    PREC_BIT_XOR,  /* ^ */
    PREC_BIT_AND,  /* & */
    PREC_SHIFT,    /* << >> */
    PREC_TERM,     /* + - */
    PREC_FACTOR,   /* * / // % */
    PREC_UNARY,    /* - ~ (prefix) */
    PREC_POWER,    /* ** (right to left) */
    PREC_AS,       /* as */
    PREC_POSTFIX,  /* calls, indexing, fields */
};

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

void plum_parser_init(PlumParser *p, const PlumTokenList *tokens, PlumArena *arena,
                      PlumInterner *names, PlumDiagList *diags) {
    p->tokens = tokens->items;
    p->count = tokens->len;
    p->pos = 0;
    p->arena = arena;
    p->names = names;
    p->diags = diags;
    p->panic = false;
}

static const PlumToken *peek(const PlumParser *p) {
    return &p->tokens[p->pos];
}

static const PlumToken *peek_at(const PlumParser *p, size_t ahead) {
    size_t i = p->pos + ahead;
    return &p->tokens[i < p->count ? i : p->count - 1];
}

static bool check(const PlumParser *p, PlumTokenKind kind) {
    return peek(p)->kind == kind;
}

static const PlumToken *advance(PlumParser *p) {
    const PlumToken *tok = peek(p);
    if (tok->kind != PLUM_TOK_EOF) {
        p->pos++;
    }
    return tok;
}

static bool match(PlumParser *p, PlumTokenKind kind) {
    if (check(p, kind)) {
        advance(p);
        return true;
    }
    return false;
}

static const PlumToken *previous(const PlumParser *p) {
    return &p->tokens[p->pos > 0 ? p->pos - 1 : 0];
}

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

/* How a token reads in a message: "'+'", "the name 'x'", "end of line". */
static void describe(const PlumToken *tok, char *buf, size_t size) {
    switch (tok->kind) {
    case PLUM_TOK_IDENT:
        snprintf(buf, size, "the name '%s'", tok->as.name);
        break;
    case PLUM_TOK_INT:
    case PLUM_TOK_FLOAT:
        snprintf(buf, size, "a number");
        break;
    case PLUM_TOK_STRING:
    case PLUM_TOK_STRING_HEAD:
        snprintf(buf, size, "a string");
        break;
    case PLUM_TOK_NEWLINE:
    case PLUM_TOK_EOF:
        snprintf(buf, size, "%s", plum_token_kind_text(tok->kind));
        break;
    default:
        snprintf(buf, size, "'%s'", plum_token_kind_text(tok->kind));
        break;
    }
}

PLUM_PRINTF(3, 4)
static void error_at(PlumParser *p, PlumSpan span, const char *fmt, ...) {
    if (p->panic) {
        return;
    }
    p->panic = true;
    char message[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    plum_diag_error(p->diags, span, "%s", message);
}

/* "expected X, found Y" at the current token. */
static void error_expected(PlumParser *p, const char *what) {
    const PlumToken *tok = peek(p);
    if (tok->kind == PLUM_TOK_ERROR) {
        p->panic = true; /* the lexer already reported it */
        return;
    }
    char found[96];
    PlumSpan at = tok->span;
    if ((tok->kind == PLUM_TOK_NEWLINE && peek_at(p, 1)->kind == PLUM_TOK_EOF) ||
        tok->kind == PLUM_TOK_EOF) {
        /* Point just after the last real token, not at the end of the file. */
        snprintf(found, sizeof found, "end of file");
        if (p->pos > 0) {
            const PlumSpan prev = previous(p)->span;
            at.offset = prev.offset + prev.length;
            at.line = prev.line;
            at.column = prev.column + prev.length; /* tokens before EOF end a line */
            at.length = 0;
        }
    } else {
        describe(tok, found, sizeof found);
    }
    error_at(p, at, "expected %s, found %s", what, found);
}

static bool expect(PlumParser *p, PlumTokenKind kind, const char *what) {
    if (match(p, kind)) {
        return true;
    }
    error_expected(p, what);
    return false;
}

/* Newlines are allowed (and skipped) inside map literals and blocks. */
static void skip_newlines(PlumParser *p) {
    while (match(p, PLUM_TOK_NEWLINE)) {
    }
}

/* ------------------------------------------------------------------ */
/* Nodes                                                               */
/* ------------------------------------------------------------------ */

static PlumSpan join(PlumSpan a, PlumSpan b) {
    PlumSpan s = a;
    uint32_t end = b.offset + b.length;
    s.length = end > a.offset ? end - a.offset : a.length;
    return s;
}

static PlumExpr *new_expr(PlumParser *p, PlumExprKind kind, PlumSpan span) {
    PlumExpr *e = plum_arena_alloc_zeroed(p->arena, sizeof *e);
    e->kind = kind;
    e->span = span;
    return e;
}

static PlumType *new_type(PlumParser *p, PlumTypeKind kind, PlumSpan span) {
    PlumType *t = plum_arena_alloc_zeroed(p->arena, sizeof *t);
    t->kind = kind;
    t->span = span;
    return t;
}

/* Copy a temporary array into the arena at its exact size. */
static void *to_arena(PlumParser *p, const void *items, size_t count, size_t size) {
    if (count == 0) {
        return NULL;
    }
    void *copy = plum_arena_alloc(p->arena, plum_mul_size(count, size));
    memcpy(copy, items, count * size);
    return copy;
}

static PlumExpr *error_expr(PlumParser *p) {
    return new_expr(p, PLUM_EXPR_ERROR, peek(p)->span);
}

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

PlumType *plum_parse_type(PlumParser *p) {
    const PlumToken *start = peek(p);
    PlumType *type;

    if (match(p, PLUM_TOK_LPAREN)) {
        /* (A, B) -> R */
        PLUM_ARRAY(PlumType *) params = {0};
        if (!check(p, PLUM_TOK_RPAREN)) {
            do {
                if (check(p, PLUM_TOK_RPAREN)) {
                    break; /* trailing comma */
                }
                plum_array_push(&params, plum_parse_type(p));
            } while (match(p, PLUM_TOK_COMMA));
        }
        expect(p, PLUM_TOK_RPAREN, "')' after the parameter types");
        expect(p, PLUM_TOK_ARROW, "'->' and a result type");
        PlumType *result = plum_parse_type(p);
        type = new_type(p, PLUM_TYPE_FUNC, join(start->span, previous(p)->span));
        type->as.func.params = to_arena(p, params.items, params.len, sizeof(PlumType *));
        type->as.func.param_count = params.len;
        type->as.func.result = result;
        plum_array_free(&params);
    } else if (check(p, PLUM_TOK_IDENT) || check(p, PLUM_TOK_NONE)) {
        const PlumToken *name = advance(p);
        type = new_type(p, PLUM_TYPE_NAME, name->span);
        type->as.name.name =
            name->kind == PLUM_TOK_NONE ? plum_intern_cstr(p->names, "none") : name->as.name;
        if (match(p, PLUM_TOK_LBRACKET)) {
            PLUM_ARRAY(PlumType *) args = {0};
            do {
                if (check(p, PLUM_TOK_RBRACKET)) {
                    break;
                }
                plum_array_push(&args, plum_parse_type(p));
            } while (match(p, PLUM_TOK_COMMA));
            expect(p, PLUM_TOK_RBRACKET, "']' after the type arguments");
            type->as.name.args = to_arena(p, args.items, args.len, sizeof(PlumType *));
            type->as.name.arg_count = args.len;
            type->span = join(name->span, previous(p)->span);
            plum_array_free(&args);
        }
    } else {
        error_expected(p, "a type");
        return new_type(p, PLUM_TYPE_ERROR, peek(p)->span);
    }

    if (check(p, PLUM_TOK_QUESTION_QUESTION)) {
        /* `T??` lexes as the ?? operator; it is a doubled optional. */
        error_at(p, peek(p)->span, "a type can be optional only once: write 'T?'");
        advance(p);
    }
    if (match(p, PLUM_TOK_QUESTION)) {
        PlumType *opt = new_type(p, PLUM_TYPE_OPTIONAL, join(type->span, previous(p)->span));
        opt->as.optional = type;
        type = opt;
        if (check(p, PLUM_TOK_QUESTION) || check(p, PLUM_TOK_QUESTION_QUESTION)) {
            error_at(p, peek(p)->span, "a type can be optional only once: write 'T?'");
        }
    }
    return type;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static PlumExpr *parse_prec(PlumParser *p, int min_prec);
static PlumStmt *parse_block(PlumParser *p);
static PlumStmt *parse_stmt(PlumParser *p);

/* The precedence of a token used as an infix operator, or PREC_NONE. */
static int infix_prec(PlumTokenKind kind) {
    switch (kind) {
    case PLUM_TOK_QUESTION_QUESTION:
        return PREC_COALESCE;
    case PLUM_TOK_OR:
        return PREC_OR;
    case PLUM_TOK_AND:
        return PREC_AND;
    case PLUM_TOK_EQ_EQ:
    case PLUM_TOK_BANG_EQ:
    case PLUM_TOK_LESS:
    case PLUM_TOK_LESS_EQ:
    case PLUM_TOK_GREATER:
    case PLUM_TOK_GREATER_EQ:
    case PLUM_TOK_IS:
    case PLUM_TOK_IN:
        return PREC_COMPARE;
    case PLUM_TOK_DOT_DOT:
    case PLUM_TOK_DOT_DOT_EQ:
        return PREC_RANGE;
    case PLUM_TOK_PIPE:
        return PREC_BIT_OR;
    case PLUM_TOK_CARET:
        return PREC_BIT_XOR;
    case PLUM_TOK_AMP:
        return PREC_BIT_AND;
    case PLUM_TOK_LESS_LESS:
    case PLUM_TOK_GREATER_GREATER:
        return PREC_SHIFT;
    case PLUM_TOK_PLUS:
    case PLUM_TOK_MINUS:
        return PREC_TERM;
    case PLUM_TOK_STAR:
    case PLUM_TOK_SLASH:
    case PLUM_TOK_SLASH_SLASH:
    case PLUM_TOK_PERCENT:
        return PREC_FACTOR;
    case PLUM_TOK_STAR_STAR:
        return PREC_POWER;
    case PLUM_TOK_AS:
        return PREC_AS;
    case PLUM_TOK_LPAREN:
    case PLUM_TOK_LBRACKET:
    case PLUM_TOK_DOT:
    case PLUM_TOK_QUESTION_DOT:
        return PREC_POSTFIX;
    default:
        return PREC_NONE;
    }
}

static PlumExpr *parse_interp(PlumParser *p) {
    const PlumToken *head = advance(p);
    PLUM_ARRAY(PlumInterpPart) parts = {0};
    if (head->as.str.len > 0) {
        PlumInterpPart text = {head->as.str.chars, head->as.str.len, NULL, NULL};
        plum_array_push(&parts, text);
    }
    for (;;) {
        PlumInterpPart part = {NULL, 0, plum_parse_expr(p), NULL};
        if (check(p, PLUM_TOK_FORMAT)) {
            part.format = advance(p)->as.str.chars;
        }
        plum_array_push(&parts, part);

        if (check(p, PLUM_TOK_STRING_MID) || check(p, PLUM_TOK_STRING_TAIL)) {
            const PlumToken *tok = advance(p);
            if (tok->as.str.len > 0) {
                PlumInterpPart text = {tok->as.str.chars, tok->as.str.len, NULL, NULL};
                plum_array_push(&parts, text);
            }
            if (tok->kind == PLUM_TOK_STRING_TAIL) {
                break;
            }
        } else {
            error_expected(p, "'}' to end the interpolation");
            /* Skip to the end of the string, which the lexer always gives. */
            while (!check(p, PLUM_TOK_STRING_TAIL) && !check(p, PLUM_TOK_EOF)) {
                advance(p);
            }
            match(p, PLUM_TOK_STRING_TAIL);
            break;
        }
    }
    PlumExpr *e = new_expr(p, PLUM_EXPR_INTERP, join(head->span, previous(p)->span));
    e->as.interp.parts = to_arena(p, parts.items, parts.len, sizeof(PlumInterpPart));
    e->as.interp.count = parts.len;
    plum_array_free(&parts);
    return e;
}

static PlumExpr *parse_list(PlumParser *p) {
    const PlumToken *open = advance(p);
    PLUM_ARRAY(PlumExpr *) items = {0};
    while (!check(p, PLUM_TOK_RBRACKET) && !check(p, PLUM_TOK_EOF)) {
        plum_array_push(&items, plum_parse_expr(p));
        if (!match(p, PLUM_TOK_COMMA)) {
            break;
        }
    }
    expect(p, PLUM_TOK_RBRACKET, "',' or ']' in the list");
    PlumExpr *e = new_expr(p, PLUM_EXPR_LIST, join(open->span, previous(p)->span));
    e->as.list.items = to_arena(p, items.items, items.len, sizeof(PlumExpr *));
    e->as.list.count = items.len;
    plum_array_free(&items);
    return e;
}

static PlumExpr *parse_map(PlumParser *p) {
    const PlumToken *open = advance(p);
    PLUM_ARRAY(PlumMapEntryExpr) entries = {0};
    skip_newlines(p);
    if (match(p, PLUM_TOK_COLON)) {
        /* {:} is the empty map */
        skip_newlines(p);
    } else {
        while (!check(p, PLUM_TOK_RBRACE) && !check(p, PLUM_TOK_EOF)) {
            PlumMapEntryExpr entry;
            entry.key = plum_parse_expr(p);
            if (!expect(p, PLUM_TOK_COLON, "':' between a map key and its value")) {
                break;
            }
            entry.value = plum_parse_expr(p);
            plum_array_push(&entries, entry);
            skip_newlines(p);
            if (!match(p, PLUM_TOK_COMMA)) {
                break;
            }
            skip_newlines(p);
        }
        if (entries.len == 0 && check(p, PLUM_TOK_RBRACE)) {
            error_at(p, join(open->span, peek(p)->span),
                     "'{}' is ambiguous here; write '{:}' for an empty map");
        }
    }
    skip_newlines(p);
    expect(p, PLUM_TOK_RBRACE, "',' or '}' in the map");
    PlumExpr *e = new_expr(p, PLUM_EXPR_MAP, join(open->span, previous(p)->span));
    e->as.map.entries = to_arena(p, entries.items, entries.len, sizeof(PlumMapEntryExpr));
    e->as.map.count = entries.len;
    plum_array_free(&entries);
    return e;
}

/* At '(': is this the parameter list of a lambda, `(...) =>`? */
static bool at_lambda(const PlumParser *p) {
    int depth = 0;
    for (size_t i = p->pos; i < p->count; i++) {
        PlumTokenKind k = p->tokens[i].kind;
        if (k == PLUM_TOK_LPAREN || k == PLUM_TOK_LBRACKET || k == PLUM_TOK_LBRACE) {
            depth++;
        } else if (k == PLUM_TOK_RPAREN || k == PLUM_TOK_RBRACKET || k == PLUM_TOK_RBRACE) {
            if (--depth == 0) {
                return i + 1 < p->count && p->tokens[i + 1].kind == PLUM_TOK_FAT_ARROW;
            }
        } else if (k == PLUM_TOK_EOF) {
            return false;
        }
    }
    return false;
}

static PlumExpr *parse_lambda(PlumParser *p) {
    const PlumToken *open = advance(p); /* ( */
    PLUM_ARRAY(PlumParam) params = {0};
    while (!check(p, PLUM_TOK_RPAREN) && !check(p, PLUM_TOK_EOF)) {
        if (!check(p, PLUM_TOK_IDENT)) {
            error_expected(p, "a parameter name");
            break;
        }
        const PlumToken *name = advance(p);
        PlumParam param = {name->as.name, NULL, NULL, name->span};
        if (match(p, PLUM_TOK_COLON)) {
            param.type = plum_parse_type(p);
        }
        plum_array_push(&params, param);
        if (!match(p, PLUM_TOK_COMMA)) {
            break;
        }
    }
    expect(p, PLUM_TOK_RPAREN, "',' or ')' after the parameters");
    expect(p, PLUM_TOK_FAT_ARROW, "'=>'");

    PlumExpr *body = NULL;
    PlumStmt *block = NULL;
    if (check(p, PLUM_TOK_LBRACE)) {
        block = parse_block(p);
    } else {
        body = plum_parse_expr(p);
    }
    PlumExpr *e = new_expr(p, PLUM_EXPR_LAMBDA, join(open->span, previous(p)->span));
    e->as.lambda.params = to_arena(p, params.items, params.len, sizeof(PlumParam));
    e->as.lambda.param_count = params.len;
    e->as.lambda.body = body;
    e->as.lambda.block = block;
    plum_array_free(&params);
    return e;
}

/* `{ expression }`: a branch of an if expression. */
static PlumExpr *parse_expr_block(PlumParser *p) {
    if (!expect(p, PLUM_TOK_LBRACE, "'{'")) {
        return error_expr(p);
    }
    skip_newlines(p);
    PlumExpr *e = plum_parse_expr(p);
    skip_newlines(p);
    expect(p, PLUM_TOK_RBRACE, "'}' after the expression");
    return e;
}

static PlumExpr *parse_if(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumExpr *cond = plum_parse_expr(p);
    PlumExpr *then = parse_expr_block(p);
    PlumExpr *otherwise = NULL;
    if (match(p, PLUM_TOK_ELSE)) {
        otherwise = check(p, PLUM_TOK_IF) ? parse_if(p) : parse_expr_block(p);
    } else {
        error_at(p, peek(p)->span, "an if expression needs an 'else' branch");
    }
    PlumExpr *e = new_expr(p, PLUM_EXPR_IF, join(kw->span, previous(p)->span));
    e->as.if_.cond = cond;
    e->as.if_.then = then;
    e->as.if_.otherwise = otherwise;
    return e;
}

/*
 * match subject {
 *     pattern, pattern => expression-or-block
 *     else => ...
 * }
 */
static PlumExpr *parse_match(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumExpr *subject = plum_parse_expr(p);
    PLUM_ARRAY(PlumMatchArm) arms = {0};
    if (expect(p, PLUM_TOK_LBRACE, "'{' to start the match arms")) {
        skip_newlines(p);
        while (!check(p, PLUM_TOK_RBRACE) && !check(p, PLUM_TOK_EOF)) {
            PlumMatchArm arm = {NULL, 0, NULL, peek(p)->span};
            PLUM_ARRAY(PlumExpr *) patterns = {0};
            if (!match(p, PLUM_TOK_ELSE)) {
                do {
                    plum_array_push(&patterns, plum_parse_expr(p));
                } while (match(p, PLUM_TOK_COMMA));
            }
            arm.patterns = to_arena(p, patterns.items, patterns.len, sizeof(PlumExpr *));
            arm.pattern_count = patterns.len;
            plum_array_free(&patterns);
            expect(p, PLUM_TOK_FAT_ARROW, "'=>' after the pattern");
            if (check(p, PLUM_TOK_LBRACE)) {
                arm.body = parse_block(p);
            } else {
                arm.body = parse_stmt(p);
            }
            arm.span = join(arm.span, previous(p)->span);
            plum_array_push(&arms, arm);
            skip_newlines(p);
            if (p->panic) {
                break;
            }
        }
        expect(p, PLUM_TOK_RBRACE, "'}' to end the match");
    }
    PlumExpr *e = new_expr(p, PLUM_EXPR_MATCH, join(kw->span, previous(p)->span));
    e->as.match.subject = subject;
    e->as.match.arms = to_arena(p, arms.items, arms.len, sizeof(PlumMatchArm));
    e->as.match.arm_count = arms.len;
    plum_array_free(&arms);
    return e;
}

static PlumExpr *parse_primary(PlumParser *p) {
    const PlumToken *tok = peek(p);
    PlumExpr *e;
    switch (tok->kind) {
    case PLUM_TOK_INT:
        advance(p);
        e = new_expr(p, PLUM_EXPR_INT, tok->span);
        e->as.i = tok->as.i;
        return e;
    case PLUM_TOK_FLOAT:
        advance(p);
        e = new_expr(p, PLUM_EXPR_FLOAT, tok->span);
        e->as.f = tok->as.f;
        return e;
    case PLUM_TOK_STRING:
        advance(p);
        e = new_expr(p, PLUM_EXPR_STRING, tok->span);
        e->as.str.chars = tok->as.str.chars;
        e->as.str.len = tok->as.str.len;
        return e;
    case PLUM_TOK_STRING_HEAD:
        return parse_interp(p);
    case PLUM_TOK_GOOD:
    case PLUM_TOK_BAD:
        advance(p);
        e = new_expr(p, PLUM_EXPR_BOOL, tok->span);
        e->as.b = tok->kind == PLUM_TOK_GOOD;
        return e;
    case PLUM_TOK_NONE:
        advance(p);
        return new_expr(p, PLUM_EXPR_NONE, tok->span);
    case PLUM_TOK_IDENT:
        advance(p);
        e = new_expr(p, PLUM_EXPR_NAME, tok->span);
        e->as.name = tok->as.name;
        return e;
    case PLUM_TOK_SELF:
        advance(p);
        return new_expr(p, PLUM_EXPR_SELF, tok->span);
    case PLUM_TOK_SUPER:
        advance(p);
        return new_expr(p, PLUM_EXPR_SUPER, tok->span);
    case PLUM_TOK_LPAREN:
        if (at_lambda(p)) {
            return parse_lambda(p);
        }
        advance(p);
        e = plum_parse_expr(p);
        expect(p, PLUM_TOK_RPAREN, "')'");
        return e;
    case PLUM_TOK_LBRACKET:
        return parse_list(p);
    case PLUM_TOK_LBRACE:
        return parse_map(p);
    case PLUM_TOK_IF:
        return parse_if(p);
    case PLUM_TOK_MATCH:
        return parse_match(p);
    case PLUM_TOK_UNDERSCORE:
        advance(p);
        return new_expr(p, PLUM_EXPR_WILDCARD, tok->span);
    case PLUM_TOK_ERROR:
        advance(p);
        p->panic = true; /* the lexer reported it */
        return new_expr(p, PLUM_EXPR_ERROR, tok->span);
    default:
        error_expected(p, "an expression");
        return error_expr(p);
    }
}

static PlumExpr *parse_prefix(PlumParser *p) {
    const PlumToken *tok = peek(p);
    switch (tok->kind) {
    case PLUM_TOK_MINUS:
    case PLUM_TOK_TILDE:
    case PLUM_TOK_NOT: {
        advance(p);
        PlumExpr *operand = parse_prec(p, tok->kind == PLUM_TOK_NOT ? PREC_NOT : PREC_UNARY);
        PlumExpr *e = new_expr(p, PLUM_EXPR_UNARY, join(tok->span, operand->span));
        e->as.unary.op = tok->kind;
        e->as.unary.operand = operand;
        return e;
    }
    case PLUM_TOK_TRY: {
        /* try binds tighter than ??: `try int(s) ?? 0` */
        advance(p);
        PlumExpr *inner = parse_prec(p, PREC_OR);
        PlumExpr *e = new_expr(p, PLUM_EXPR_TRY, join(tok->span, inner->span));
        e->as.inner = inner;
        return e;
    }
    case PLUM_TOK_SPAWN: {
        advance(p);
        PlumExpr *inner = parse_prec(p, PREC_POSTFIX);
        if (inner->kind != PLUM_EXPR_CALL && inner->kind != PLUM_EXPR_ERROR) {
            error_at(p, inner->span, "spawn needs a function call, like 'spawn work()'");
        }
        PlumExpr *e = new_expr(p, PLUM_EXPR_SPAWN, join(tok->span, inner->span));
        e->as.inner = inner;
        return e;
    }
    default:
        return parse_primary(p);
    }
}

static PlumExpr *parse_call(PlumParser *p, PlumExpr *callee) {
    PLUM_ARRAY(PlumArg) args = {0};
    bool named_seen = false;
    while (!check(p, PLUM_TOK_RPAREN) && !check(p, PLUM_TOK_EOF)) {
        PlumArg arg = {NULL, NULL, peek(p)->span};
        if (check(p, PLUM_TOK_IDENT) && peek_at(p, 1)->kind == PLUM_TOK_COLON) {
            arg.name = advance(p)->as.name;
            advance(p); /* : */
            named_seen = true;
        } else if (named_seen) {
            error_at(p, peek(p)->span, "positional arguments must come before named ones");
        }
        arg.value = plum_parse_expr(p);
        arg.span = join(arg.span, arg.value->span);
        plum_array_push(&args, arg);
        if (!match(p, PLUM_TOK_COMMA)) {
            break;
        }
    }
    expect(p, PLUM_TOK_RPAREN, "',' or ')' in the call");
    PlumExpr *e = new_expr(p, PLUM_EXPR_CALL, join(callee->span, previous(p)->span));
    e->as.call.callee = callee;
    e->as.call.args = to_arena(p, args.items, args.len, sizeof(PlumArg));
    e->as.call.arg_count = args.len;
    plum_array_free(&args);
    return e;
}

static bool is_comparison(PlumTokenKind k) {
    return infix_prec(k) == PREC_COMPARE;
}

static PlumExpr *parse_prec(PlumParser *p, int min_prec) {
    PlumExpr *left = parse_prefix(p);

    for (;;) {
        const PlumToken *op = peek(p);
        int prec = infix_prec(op->kind);
        if (prec == PREC_NONE || prec < min_prec) {
            return left;
        }
        advance(p);

        switch (op->kind) {
        case PLUM_TOK_LPAREN:
            left = parse_call(p, left);
            continue;
        case PLUM_TOK_LBRACKET: {
            PlumExpr *index = plum_parse_expr(p);
            expect(p, PLUM_TOK_RBRACKET, "']'");
            PlumExpr *e = new_expr(p, PLUM_EXPR_INDEX, join(left->span, previous(p)->span));
            e->as.index.object = left;
            e->as.index.index = index;
            left = e;
            continue;
        }
        case PLUM_TOK_DOT:
        case PLUM_TOK_QUESTION_DOT: {
            if (!check(p, PLUM_TOK_IDENT)) {
                error_expected(p, "a field or method name after '.'");
                return left;
            }
            const PlumToken *name = advance(p);
            PlumExpr *e = new_expr(p, PLUM_EXPR_FIELD, join(left->span, name->span));
            e->as.field.object = left;
            e->as.field.name = name->as.name;
            e->as.field.optional = op->kind == PLUM_TOK_QUESTION_DOT;
            left = e;
            continue;
        }
        case PLUM_TOK_AS:
        case PLUM_TOK_IS: {
            PlumType *type = plum_parse_type(p);
            PlumExpr *e = new_expr(p, op->kind == PLUM_TOK_AS ? PLUM_EXPR_AS : PLUM_EXPR_IS,
                                   join(left->span, type->span));
            e->as.cast.value = left;
            e->as.cast.type = type;
            left = e;
            break;
        }
        case PLUM_TOK_DOT_DOT:
        case PLUM_TOK_DOT_DOT_EQ: {
            PlumExpr *high = parse_prec(p, prec + 1);
            PlumExpr *e = new_expr(p, PLUM_EXPR_RANGE, join(left->span, high->span));
            e->as.range.low = left;
            e->as.range.high = high;
            e->as.range.inclusive = op->kind == PLUM_TOK_DOT_DOT_EQ;
            left = e;
            if (check(p, PLUM_TOK_DOT_DOT) || check(p, PLUM_TOK_DOT_DOT_EQ)) {
                error_at(p, peek(p)->span, "ranges can't be chained");
            }
            continue;
        }
        default: {
            /* ** groups right to left; everything else left to right. */
            int next = op->kind == PLUM_TOK_STAR_STAR ? PREC_UNARY : prec + 1;
            PlumExpr *right = parse_prec(p, next);
            PlumExpr *e = new_expr(p, PLUM_EXPR_BINARY, join(left->span, right->span));
            e->as.binary.op = op->kind;
            e->as.binary.left = left;
            e->as.binary.right = right;
            left = e;
            break;
        }
        }

        if (prec == PREC_COMPARE && is_comparison(peek(p)->kind)) {
            error_at(p, peek(p)->span,
                     "comparisons can't be chained; write 'a < b and b < c' instead");
        }
    }
}

PlumExpr *plum_parse_expr(PlumParser *p) {
    return parse_prec(p, PREC_COALESCE);
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static PlumStmt *new_stmt(PlumParser *p, PlumStmtKind kind, PlumSpan span) {
    PlumStmt *s = plum_arena_alloc_zeroed(p->arena, sizeof *s);
    s->kind = kind;
    s->span = span;
    return s;
}

/* Finish a statement's span at the last token it used. */
static PlumStmt *finish(PlumParser *p, PlumStmt *s) {
    s->span = join(s->span, previous(p)->span);
    return s;
}

/*
 * After an error, skip to the end of the statement: the next newline or
 * '}' outside any brackets opened while skipping. Braces stay balanced, so
 * the enclosing block still ends where it should.
 */
static void synchronize(PlumParser *p) {
    if (previous(p)->kind == PLUM_TOK_NEWLINE) {
        /* The statement with the error still ended at its newline; the
         * next statement is fine to parse. */
        p->panic = false;
        return;
    }
    int depth = 0;
    while (!check(p, PLUM_TOK_EOF)) {
        PlumTokenKind k = peek(p)->kind;
        if (depth == 0 && (k == PLUM_TOK_NEWLINE || k == PLUM_TOK_RBRACE)) {
            break;
        }
        if (k == PLUM_TOK_LBRACE || k == PLUM_TOK_LPAREN || k == PLUM_TOK_LBRACKET) {
            depth++;
        } else if ((k == PLUM_TOK_RBRACE || k == PLUM_TOK_RPAREN || k == PLUM_TOK_RBRACKET) &&
                   depth > 0) {
            depth--;
        }
        advance(p);
    }
    p->panic = false;
}

/* A statement ends at a newline, or just before '}' or the end of the file. */
static void end_statement(PlumParser *p) {
    if (match(p, PLUM_TOK_NEWLINE) || check(p, PLUM_TOK_RBRACE) || check(p, PLUM_TOK_EOF)) {
        return;
    }
    error_expected(p, "the end of the statement");
}

static const char *expect_name(PlumParser *p, const char *what) {
    if (check(p, PLUM_TOK_IDENT)) {
        return advance(p)->as.name;
    }
    error_expected(p, what);
    return plum_intern_cstr(p->names, "<error>");
}

/* `{ statements }` */
static PlumStmt *parse_block(PlumParser *p) {
    const PlumToken *open = peek(p);
    PlumStmt *block = new_stmt(p, PLUM_STMT_BLOCK, open->span);
    if (!expect(p, PLUM_TOK_LBRACE, "'{'")) {
        return block;
    }
    PLUM_ARRAY(PlumStmt *) stmts = {0};
    skip_newlines(p);
    while (!check(p, PLUM_TOK_RBRACE) && !check(p, PLUM_TOK_EOF)) {
        PlumStmt *s = parse_stmt(p);
        plum_array_push(&stmts, s);
        if (p->panic) {
            synchronize(p);
        }
        skip_newlines(p);
    }
    expect(p, PLUM_TOK_RBRACE, "'}' to close the block");
    block->as.block.stmts = to_arena(p, stmts.items, stmts.len, sizeof(PlumStmt *));
    block->as.block.count = stmts.len;
    plum_array_free(&stmts);
    return finish(p, block);
}

/* `else` may follow '}' on the same line or on the next one. */
static bool match_else(PlumParser *p) {
    if (check(p, PLUM_TOK_NEWLINE) && peek_at(p, 1)->kind == PLUM_TOK_ELSE) {
        advance(p);
    }
    return match(p, PLUM_TOK_ELSE);
}

static PlumStmt *parse_if_stmt(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumStmt *s;
    if (match(p, PLUM_TOK_PLANT)) {
        /* if plant name = optional { } */
        s = new_stmt(p, PLUM_STMT_IF_PLANT, kw->span);
        s->as.if_.name = expect_name(p, "a name after 'if plant'");
        expect(p, PLUM_TOK_EQ, "'=' and the optional value to unwrap");
        s->as.if_.value = plum_parse_expr(p);
    } else {
        s = new_stmt(p, PLUM_STMT_IF, kw->span);
        s->as.if_.cond = plum_parse_expr(p);
    }
    s->as.if_.then = parse_block(p);
    if (match_else(p)) {
        s->as.if_.otherwise = check(p, PLUM_TOK_IF) ? parse_if_stmt(p) : parse_block(p);
    }
    return finish(p, s);
}

static PlumStmt *parse_for(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumStmt *s = new_stmt(p, PLUM_STMT_FOR, kw->span);
    const char *wildcard = plum_intern_cstr(p->names, "_");
    s->as.for_.first = match(p, PLUM_TOK_UNDERSCORE) ? wildcard : expect_name(p, "a loop name");
    if (match(p, PLUM_TOK_COMMA)) {
        s->as.for_.second =
            match(p, PLUM_TOK_UNDERSCORE) ? wildcard : expect_name(p, "a second loop name");
    }
    expect(p, PLUM_TOK_IN, "'in'");
    s->as.for_.iterable = plum_parse_expr(p);
    s->as.for_.body = parse_block(p);
    return finish(p, s);
}

static PlumStmt *parse_try_stmt(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumStmt *s = new_stmt(p, PLUM_STMT_TRY, kw->span);
    s->as.try_.body = parse_block(p);
    PLUM_ARRAY(PlumCatch) catches = {0};
    for (;;) {
        if (check(p, PLUM_TOK_NEWLINE) && peek_at(p, 1)->kind == PLUM_TOK_CATCH) {
            advance(p);
        }
        if (!check(p, PLUM_TOK_CATCH)) {
            break;
        }
        const PlumToken *c = advance(p);
        PlumCatch handler = {NULL, NULL, NULL, c->span};
        handler.name = expect_name(p, "a name for the error, like 'catch e'");
        if (match(p, PLUM_TOK_COLON)) {
            handler.type = plum_parse_type(p);
        }
        handler.body = parse_block(p);
        handler.span = join(c->span, previous(p)->span);
        plum_array_push(&catches, handler);
    }
    if (check(p, PLUM_TOK_NEWLINE) && peek_at(p, 1)->kind == PLUM_TOK_FINALLY) {
        advance(p);
    }
    if (match(p, PLUM_TOK_FINALLY)) {
        s->as.try_.finally = parse_block(p);
    }
    if (catches.len == 0 && s->as.try_.finally == NULL) {
        error_at(p, kw->span, "a try block needs at least one 'catch' or a 'finally'");
    }
    s->as.try_.catches = to_arena(p, catches.items, catches.len, sizeof(PlumCatch));
    s->as.try_.catch_count = catches.len;
    plum_array_free(&catches);
    return finish(p, s);
}

/* `(name: type = default, ...)` for functions and methods. */
static void parse_params(PlumParser *p, PlumParam **out, size_t *count) {
    PLUM_ARRAY(PlumParam) params = {0};
    expect(p, PLUM_TOK_LPAREN, "'(' and the parameters");
    bool defaults_seen = false;
    while (!check(p, PLUM_TOK_RPAREN) && !check(p, PLUM_TOK_EOF)) {
        const PlumToken *name = peek(p);
        PlumParam param = {expect_name(p, "a parameter name"), NULL, NULL, name->span};
        if (expect(p, PLUM_TOK_COLON, "':' and the parameter's type")) {
            param.type = plum_parse_type(p);
        }
        if (match(p, PLUM_TOK_EQ)) {
            param.fallback = plum_parse_expr(p);
            defaults_seen = true;
        } else if (defaults_seen && !p->panic) {
            error_at(p, name->span, "parameters with defaults must come last");
        }
        param.span = join(name->span, previous(p)->span);
        plum_array_push(&params, param);
        if (!match(p, PLUM_TOK_COMMA) || p->panic) {
            break;
        }
    }
    expect(p, PLUM_TOK_RPAREN, "',' or ')' after the parameters");
    *out = to_arena(p, params.items, params.len, sizeof(PlumParam));
    *count = params.len;
    plum_array_free(&params);
}

/*
 * branch name(params) -> Result { body }
 * branch name(params) -> Result => expression
 * branch name(params) -> Result          (trait methods only: no body)
 */
static PlumBranch parse_branch_decl(PlumParser *p, bool body_optional) {
    PlumBranch b;
    memset(&b, 0, sizeof b);
    b.name = expect_name(p, "the function's name");
    parse_params(p, &b.params, &b.param_count);
    if (match(p, PLUM_TOK_ARROW)) {
        b.result = plum_parse_type(p);
    }
    if (match(p, PLUM_TOK_FAT_ARROW)) {
        b.arrow = plum_parse_expr(p);
    } else if (check(p, PLUM_TOK_LBRACE)) {
        b.body = parse_block(p);
    } else if (!body_optional) {
        error_expected(p, "'{' or '=>' and the function's body");
    }
    return b;
}

static PlumStmt *parse_branch(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumStmt *s = new_stmt(p, PLUM_STMT_BRANCH, kw->span);
    s->as.branch = parse_branch_decl(p, false);
    return finish(p, s);
}

/* tree Name : Parent, Trait { fields and methods } -- and trait, without fields. */
static PlumStmt *parse_tree(PlumParser *p, bool is_trait) {
    const PlumToken *kw = advance(p);
    PlumStmt *s = new_stmt(p, is_trait ? PLUM_STMT_TRAIT : PLUM_STMT_TREE, kw->span);
    s->as.tree.name = expect_name(p, is_trait ? "the trait's name" : "the tree's name");

    PLUM_ARRAY(PlumType *) supers = {0};
    if (match(p, PLUM_TOK_COLON)) {
        do {
            plum_array_push(&supers, plum_parse_type(p));
        } while (match(p, PLUM_TOK_COMMA));
    }

    PLUM_ARRAY(PlumField) fields = {0};
    PLUM_ARRAY(PlumBranch) methods = {0};
    if (expect(p, PLUM_TOK_LBRACE, "'{' to start the body")) {
        skip_newlines(p);
        while (!check(p, PLUM_TOK_RBRACE) && !check(p, PLUM_TOK_EOF)) {
            if (match(p, PLUM_TOK_BRANCH)) {
                plum_array_push(&methods, parse_branch_decl(p, is_trait));
                end_statement(p);
            } else if (check(p, PLUM_TOK_IDENT) && !is_trait) {
                const PlumToken *name = advance(p);
                PlumField field = {name->as.name, NULL, NULL, false, name->span};
                if (expect(p, PLUM_TOK_COLON, "':' and the field's type")) {
                    field.weak = match(p, PLUM_TOK_WEAK);
                    field.type = plum_parse_type(p);
                    if (match(p, PLUM_TOK_EQ)) {
                        field.fallback = plum_parse_expr(p);
                    }
                }
                field.span = join(name->span, previous(p)->span);
                plum_array_push(&fields, field);
                end_statement(p);
            } else if ((check(p, PLUM_TOK_PLANT) || check(p, PLUM_TOK_ROOT)) && !is_trait) {
                error_at(p, peek(p)->span, "fields are written without '%s': 'name: type = value'",
                         plum_token_kind_text(peek(p)->kind));
            } else {
                error_expected(p, is_trait ? "a method ('branch ...')"
                                           : "a field ('name: type') or a method ('branch ...')");
            }
            if (p->panic) {
                synchronize(p);
            }
            skip_newlines(p);
        }
        expect(p, PLUM_TOK_RBRACE, "'}' to end the body");
    }

    s->as.tree.supers = to_arena(p, supers.items, supers.len, sizeof(PlumType *));
    s->as.tree.super_count = supers.len;
    s->as.tree.fields = to_arena(p, fields.items, fields.len, sizeof(PlumField));
    s->as.tree.field_count = fields.len;
    s->as.tree.methods = to_arena(p, methods.items, methods.len, sizeof(PlumBranch));
    s->as.tree.method_count = methods.len;
    plum_array_free(&supers);
    plum_array_free(&fields);
    plum_array_free(&methods);
    return finish(p, s);
}

/* kind Name { Case, Case(name: type), ... } -- cases split by ',' or newlines. */
static PlumStmt *parse_kind(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumStmt *s = new_stmt(p, PLUM_STMT_KIND, kw->span);
    s->as.kind.name = expect_name(p, "the kind's name");
    PLUM_ARRAY(PlumCase) cases = {0};
    if (expect(p, PLUM_TOK_LBRACE, "'{' and the cases")) {
        skip_newlines(p);
        while (!check(p, PLUM_TOK_RBRACE) && !check(p, PLUM_TOK_EOF)) {
            const PlumToken *name = peek(p);
            PlumCase c = {expect_name(p, "a case name"), NULL, 0, name->span};
            if (check(p, PLUM_TOK_LPAREN)) {
                parse_params(p, &c.params, &c.param_count);
            }
            c.span = join(name->span, previous(p)->span);
            plum_array_push(&cases, c);
            if (p->panic) {
                synchronize(p);
            }
            if (!match(p, PLUM_TOK_COMMA) && !check(p, PLUM_TOK_NEWLINE)) {
                break;
            }
            skip_newlines(p);
        }
        skip_newlines(p);
        expect(p, PLUM_TOK_RBRACE, "',' or '}' after the case");
    }
    if (cases.len == 0 && !p->panic) {
        error_at(p, kw->span, "a kind needs at least one case");
    }
    s->as.kind.cases = to_arena(p, cases.items, cases.len, sizeof(PlumCase));
    s->as.kind.case_count = cases.len;
    plum_array_free(&cases);
    return finish(p, s);
}

/*
 * seed math | seed net.http as web | seed ./models/user | seed ../lib/x
 * seed math { sqrt, PI }
 */
static PlumStmt *parse_seed(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumStmt *s = new_stmt(p, PLUM_STMT_SEED, kw->span);
    PLUM_ARRAY(char) path = {0};

    if (check(p, PLUM_TOK_DOT) || check(p, PLUM_TOK_DOT_DOT)) {
        /* A relative path: ./a/b or ../a/b, segments joined by '/'. */
        while (check(p, PLUM_TOK_DOT) || check(p, PLUM_TOK_DOT_DOT)) {
            const char *dots = advance(p)->kind == PLUM_TOK_DOT ? "./" : "../";
            if (!expect(p, PLUM_TOK_SLASH, "'/' in the path")) {
                break;
            }
            for (const char *c = dots; *c; c++) {
                plum_array_push(&path, *c);
            }
        }
        do {
            const char *seg = expect_name(p, "a file name in the path");
            for (const char *c = seg; *c; c++) {
                plum_array_push(&path, *c);
            }
            if (!check(p, PLUM_TOK_SLASH)) {
                break;
            }
            advance(p);
            plum_array_push(&path, '/');
        } while (!p->panic);
    } else {
        do {
            const char *seg = expect_name(p, "a module name");
            if (path.len > 0) {
                plum_array_push(&path, '.');
            }
            for (const char *c = seg; *c; c++) {
                plum_array_push(&path, *c);
            }
        } while (match(p, PLUM_TOK_DOT) && !p->panic);
    }
    s->as.seed.path = plum_intern(p->names, path.items ? path.items : "", path.len);
    plum_array_free(&path);

    if (match(p, PLUM_TOK_AS)) {
        s->as.seed.alias = expect_name(p, "a name after 'as'");
    }
    if (match(p, PLUM_TOK_LBRACE)) {
        PLUM_ARRAY(const char *) names = {0};
        while (!check(p, PLUM_TOK_RBRACE) && !check(p, PLUM_TOK_EOF)) {
            plum_array_push(&names, expect_name(p, "a name to import"));
            if (!match(p, PLUM_TOK_COMMA) || p->panic) {
                break;
            }
        }
        expect(p, PLUM_TOK_RBRACE, "',' or '}' after the names");
        s->as.seed.names = to_arena(p, names.items, names.len, sizeof(const char *));
        s->as.seed.name_count = names.len;
        plum_array_free(&names);
    }
    return finish(p, s);
}

static bool is_assign_op(PlumTokenKind k) {
    switch (k) {
    case PLUM_TOK_EQ:
    case PLUM_TOK_PLUS_EQ:
    case PLUM_TOK_MINUS_EQ:
    case PLUM_TOK_STAR_EQ:
    case PLUM_TOK_SLASH_EQ:
    case PLUM_TOK_SLASH_SLASH_EQ:
    case PLUM_TOK_PERCENT_EQ:
    case PLUM_TOK_STAR_STAR_EQ:
    case PLUM_TOK_AMP_EQ:
    case PLUM_TOK_PIPE_EQ:
    case PLUM_TOK_CARET_EQ:
    case PLUM_TOK_LESS_LESS_EQ:
    case PLUM_TOK_GREATER_GREATER_EQ:
        return true;
    default:
        return false;
    }
}

/* An expression statement, or an assignment if '=' or `op=` follows. */
static PlumStmt *parse_expr_or_assign(PlumParser *p) {
    PlumExpr *target = plum_parse_expr(p);
    if (!is_assign_op(peek(p)->kind)) {
        PlumStmt *s = new_stmt(p, PLUM_STMT_EXPR, target->span);
        s->as.expr = target;
        return s;
    }
    const PlumToken *op = advance(p);
    bool assignable = target->kind == PLUM_EXPR_NAME || target->kind == PLUM_EXPR_INDEX ||
                      (target->kind == PLUM_EXPR_FIELD && !target->as.field.optional) ||
                      target->kind == PLUM_EXPR_ERROR;
    if (!assignable) {
        error_at(p, target->span,
                 "can't assign to this; the left of '%s' must be a name, a field or an index",
                 plum_token_kind_text(op->kind));
    }
    PlumStmt *s = new_stmt(p, PLUM_STMT_ASSIGN, target->span);
    s->as.assign.op = op->kind;
    s->as.assign.target = target;
    s->as.assign.value = plum_parse_expr(p);
    return finish(p, s);
}

static PlumStmt *parse_decl(PlumParser *p) {
    const PlumToken *kw = advance(p);
    PlumStmt *s = new_stmt(p, PLUM_STMT_DECL, kw->span);
    s->as.decl.constant = kw->kind == PLUM_TOK_ROOT;
    s->as.decl.name = expect_name(p, "a name");
    if (match(p, PLUM_TOK_COLON)) {
        s->as.decl.type = plum_parse_type(p);
    }
    if (match(p, PLUM_TOK_EQ)) {
        s->as.decl.value = plum_parse_expr(p);
    } else if (!p->panic) {
        const char *word = plum_token_kind_text(kw->kind);
        error_at(p, peek(p)->span, "'%s %s' needs a value: %s %s = ...", word, s->as.decl.name,
                 word, s->as.decl.name);
    }
    return finish(p, s);
}

static PlumStmt *parse_stmt(PlumParser *p) {
    const PlumToken *tok = peek(p);
    PlumStmt *s;
    switch (tok->kind) {
    case PLUM_TOK_PLANT:
    case PLUM_TOK_ROOT:
        s = parse_decl(p);
        break;
    case PLUM_TOK_IF:
        return parse_if_stmt(p);
    case PLUM_TOK_WHILE: {
        advance(p);
        s = new_stmt(p, PLUM_STMT_WHILE, tok->span);
        s->as.while_.cond = plum_parse_expr(p);
        s->as.while_.body = parse_block(p);
        return finish(p, s);
    }
    case PLUM_TOK_FOR:
        return parse_for(p);
    case PLUM_TOK_BREAK:
    case PLUM_TOK_CONTINUE:
        advance(p);
        s = new_stmt(p, tok->kind == PLUM_TOK_BREAK ? PLUM_STMT_BREAK : PLUM_STMT_CONTINUE,
                     tok->span);
        break;
    case PLUM_TOK_RETURN:
        advance(p);
        s = new_stmt(p, PLUM_STMT_RETURN, tok->span);
        if (!check(p, PLUM_TOK_NEWLINE) && !check(p, PLUM_TOK_RBRACE) && !check(p, PLUM_TOK_EOF)) {
            s->as.expr = plum_parse_expr(p);
        }
        finish(p, s);
        break;
    case PLUM_TOK_THROW:
        advance(p);
        s = new_stmt(p, PLUM_STMT_THROW, tok->span);
        s->as.expr = plum_parse_expr(p);
        finish(p, s);
        break;
    case PLUM_TOK_TRY:
        if (peek_at(p, 1)->kind == PLUM_TOK_LBRACE) {
            return parse_try_stmt(p);
        }
        s = parse_expr_or_assign(p); /* `try risky()` */
        break;
    case PLUM_TOK_BRANCH:
        return parse_branch(p);
    case PLUM_TOK_TREE:
        return parse_tree(p, false);
    case PLUM_TOK_TRAIT:
        return parse_tree(p, true);
    case PLUM_TOK_KIND:
        return parse_kind(p);
    case PLUM_TOK_SEED:
        s = parse_seed(p);
        break;
    case PLUM_TOK_LBRACE:
        error_at(p, tok->span,
                 "a block can't stand on its own; for an empty map write 'plant m = {:}'");
        s = new_stmt(p, PLUM_STMT_ERROR, tok->span);
        break;
    default:
        s = parse_expr_or_assign(p);
        break;
    }
    end_statement(p);
    return s;
}

PlumProgram plum_parse_program(PlumParser *p) {
    PLUM_ARRAY(PlumStmt *) stmts = {0};
    bool other_seen = false;
    skip_newlines(p);
    while (!check(p, PLUM_TOK_EOF)) {
        if (check(p, PLUM_TOK_RBRACE)) {
            /* The lexer reported the unmatched '}'; skip it. */
            advance(p);
            skip_newlines(p);
            continue;
        }
        PlumStmt *s = parse_stmt(p);
        if (s->kind == PLUM_STMT_SEED && other_seen) {
            plum_diag_error(p->diags, s->span,
                            "'seed' must come before other statements; move it to the top");
        } else if (s->kind != PLUM_STMT_SEED) {
            other_seen = true;
        }
        plum_array_push(&stmts, s);
        if (p->panic) {
            synchronize(p);
        }
        skip_newlines(p);
    }
    PlumProgram program = {to_arena(p, stmts.items, stmts.len, sizeof(PlumStmt *)), stmts.len};
    plum_array_free(&stmts);
    return program;
}
