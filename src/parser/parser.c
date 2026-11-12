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
    describe(tok, found, sizeof found);
    error_at(p, tok->span, "expected %s, found %s", what, found);
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

    if (match(p, PLUM_TOK_QUESTION)) {
        PlumType *opt = new_type(p, PLUM_TYPE_OPTIONAL, join(type->span, previous(p)->span));
        opt->as.optional = type;
        type = opt;
        if (check(p, PLUM_TOK_QUESTION)) {
            error_at(p, peek(p)->span, "a type can be optional only once: write 'T?'");
        }
    }
    return type;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static PlumExpr *parse_prec(PlumParser *p, int min_prec);

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

    PlumExpr *body;
    if (check(p, PLUM_TOK_LBRACE)) {
        /* Block bodies are statements, which arrive with #10. */
        error_at(p, peek(p)->span, "lambdas with a { } body are not supported yet (#10)");
        body = error_expr(p);
    } else {
        body = plum_parse_expr(p);
    }
    PlumExpr *e = new_expr(p, PLUM_EXPR_LAMBDA, join(open->span, previous(p)->span));
    e->as.lambda.params = to_arena(p, params.items, params.len, sizeof(PlumParam));
    e->as.lambda.param_count = params.len;
    e->as.lambda.body = body;
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
/* Lines of expressions (until statements arrive)                      */
/* ------------------------------------------------------------------ */

PlumExpr **plum_parse_expr_lines(PlumParser *p, size_t *count) {
    PLUM_ARRAY(PlumExpr *) exprs = {0};
    skip_newlines(p);
    while (!check(p, PLUM_TOK_EOF)) {
        plum_array_push(&exprs, plum_parse_expr(p));
        if (!check(p, PLUM_TOK_NEWLINE)) {
            error_expected(p, "the end of the line");
        }
        /* Resynchronise at the next line. */
        while (!check(p, PLUM_TOK_NEWLINE) && !check(p, PLUM_TOK_EOF)) {
            advance(p);
        }
        skip_newlines(p);
        p->panic = false;
    }
    *count = exprs.len;
    PlumExpr **out = plum_arena_alloc(p->arena, (exprs.len + 1) * sizeof(PlumExpr *));
    if (exprs.len > 0) {
        memcpy(out, exprs.items, exprs.len * sizeof(PlumExpr *));
    }
    out[exprs.len] = NULL;
    plum_array_free(&exprs);
    return out;
}
