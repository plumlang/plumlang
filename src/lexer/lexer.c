/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "lexer/lexer.h"

#include "util/alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* An open bracket, or an open string interpolation, waiting to be closed. */
typedef struct Open {
    char kind;     /* '(' '[' '{', or 'I' for an interpolation */
    bool triple;   /* 'I': the string is a """ string */
    size_t indent; /* 'I' in a """ string: indentation to remove */
    PlumSpan span; /* the opening bracket, or the string's opening quote */
} Open;

typedef struct Lexer {
    const PlumSource *source;
    const char *text;
    size_t length;
    size_t pos; /* byte offset of the next character */
    uint32_t line;
    uint32_t column;

    PlumArena *arena;
    PlumInterner *names;
    PlumDiagList *diags;
    PlumTokenList *out;

    PLUM_ARRAY(Open) open;
    PLUM_ARRAY(char) buf; /* decoded string text being built */
} Lexer;

/* ------------------------------------------------------------------ */
/* Characters and positions                                            */
/* ------------------------------------------------------------------ */

static bool at_end(const Lexer *lx) {
    return lx->pos >= lx->length;
}

static char peek(const Lexer *lx) {
    return at_end(lx) ? '\0' : lx->text[lx->pos];
}

static char peek_at(const Lexer *lx, size_t ahead) {
    return lx->pos + ahead < lx->length ? lx->text[lx->pos + ahead] : '\0';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

/*
 * Decode one UTF-8 character at s[0..n). Returns its length in bytes and
 * stores the code point, or returns 0 for an invalid sequence.
 */
static size_t utf8_decode(const unsigned char *s, size_t n, uint32_t *cp) {
    if (n == 0) {
        return 0;
    }
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t len;
    uint32_t value;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) {
        len = 2, value = c & 0x1F, min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3, value = c & 0x0F, min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4, value = c & 0x07, min = 0x10000;
    } else {
        return 0;
    }
    if (n < len) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        value = (value << 6) | (s[i] & 0x3F);
    }
    if (value < min || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return 0; /* overlong, out of range, or a surrogate */
    }
    *cp = value;
    return len;
}

static size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The span of a zero-length point at the current position. */
static PlumSpan here(const Lexer *lx) {
    PlumSpan s = {(uint32_t)lx->pos, 0, lx->line, lx->column};
    return s;
}

/* Extend `start` to the current position. */
static PlumSpan span_from(const Lexer *lx, PlumSpan start) {
    start.length = (uint32_t)(lx->pos - start.offset);
    return start;
}

/*
 * Move past one character, keeping line and column. Returns bad if the
 * bytes are not valid UTF-8 (one byte is skipped and reported).
 */
static bool advance(Lexer *lx) {
    if (at_end(lx)) {
        return true;
    }
    char c = lx->text[lx->pos];
    if (c == '\n') {
        lx->pos++;
        lx->line++;
        lx->column = 1;
        return true;
    }
    if ((unsigned char)c < 0x80) {
        lx->pos++;
        lx->column++;
        return true;
    }
    uint32_t cp;
    size_t n = utf8_decode((const unsigned char *)lx->text + lx->pos, lx->length - lx->pos, &cp);
    if (n == 0) {
        PlumSpan at = here(lx);
        at.length = 1;
        plum_diag_error(lx->diags, at, "invalid UTF-8 byte 0x%02X; source files must be UTF-8",
                        (unsigned)(unsigned char)c);
        lx->pos++;
        lx->column++;
        return false;
    }
    lx->pos += n;
    lx->column++;
    return true;
}

/* ------------------------------------------------------------------ */
/* Emitting tokens                                                     */
/* ------------------------------------------------------------------ */

static PlumToken *emit(Lexer *lx, PlumTokenKind kind, PlumSpan span) {
    PlumToken tok;
    memset(&tok, 0, sizeof tok);
    tok.kind = kind;
    tok.span = span;
    plum_array_push(lx->out, tok);
    return &plum_array_last(lx->out);
}

static PlumTokenKind last_kind(const Lexer *lx) {
    return lx->out->len == 0 ? PLUM_TOK_NEWLINE : plum_array_last(lx->out).kind;
}

static char open_top(const Lexer *lx) {
    return lx->open.len == 0 ? '\0' : plum_array_last(&lx->open).kind;
}

/* True if a statement cannot end after this token, so a newline is skipped. */
static bool continues_line(PlumTokenKind kind) {
    switch (kind) {
    case PLUM_TOK_NEWLINE:
    case PLUM_TOK_LBRACE:
    case PLUM_TOK_COMMA:
    case PLUM_TOK_DOT:
    case PLUM_TOK_QUESTION_DOT:
    case PLUM_TOK_COLON:
    case PLUM_TOK_ARROW:
    case PLUM_TOK_FAT_ARROW:
    case PLUM_TOK_DOT_DOT:
    case PLUM_TOK_DOT_DOT_EQ:
    case PLUM_TOK_QUESTION_QUESTION:
    case PLUM_TOK_PLUS:
    case PLUM_TOK_MINUS:
    case PLUM_TOK_STAR:
    case PLUM_TOK_SLASH:
    case PLUM_TOK_SLASH_SLASH:
    case PLUM_TOK_PERCENT:
    case PLUM_TOK_STAR_STAR:
    case PLUM_TOK_AMP:
    case PLUM_TOK_PIPE:
    case PLUM_TOK_CARET:
    case PLUM_TOK_TILDE:
    case PLUM_TOK_LESS_LESS:
    case PLUM_TOK_GREATER_GREATER:
    case PLUM_TOK_EQ_EQ:
    case PLUM_TOK_BANG_EQ:
    case PLUM_TOK_LESS:
    case PLUM_TOK_LESS_EQ:
    case PLUM_TOK_GREATER:
    case PLUM_TOK_GREATER_EQ:
    case PLUM_TOK_AND:
    case PLUM_TOK_OR:
    case PLUM_TOK_NOT:
    case PLUM_TOK_IN:
    case PLUM_TOK_IS:
    case PLUM_TOK_AS:
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

/* Keywords that can only begin a statement. */
static bool starts_statement(const char *s, size_t n) {
    static const char *const words[] = {"plant", "root",     "branch", "tree",  "trait",
                                        "kind",  "seed",     "for",    "while", "return",
                                        "break", "continue", "throw"};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        size_t len = strlen(words[i]);
        if (n > len && memcmp(s, words[i], len) == 0 && !is_alnum(s[len])) {
            return true;
        }
    }
    return false;
}

/*
 * Inside an unclosed ( or [, a line that begins a new statement means the
 * bracket was never closed. Report it at the bracket and close it, so the
 * error points at the real mistake instead of a later line.
 */
static void close_forgotten_brackets(Lexer *lx) {
    size_t next = lx->pos + 1;
    while (next < lx->length && (lx->text[next] == ' ' || lx->text[next] == '\t')) {
        next++;
    }
    if (!starts_statement(lx->text + next, lx->length - next)) {
        return;
    }
    while (lx->open.len > 0 && (open_top(lx) == '(' || open_top(lx) == '[')) {
        Open *o = &plum_array_last(&lx->open);
        plum_diag_error(lx->diags, o->span, "'%c' is never closed", o->kind);
        lx->open.len--;
    }
}

static void newline(Lexer *lx) {
    PlumSpan at = here(lx);
    at.length = 1;
    if (open_top(lx) == '(' || open_top(lx) == '[') {
        close_forgotten_brackets(lx);
    }
    char top = open_top(lx);
    bool inside = top == '(' || top == '[' || top == 'I';
    if (!inside && !continues_line(last_kind(lx))) {
        emit(lx, PLUM_TOK_NEWLINE, at);
    }
    advance(lx);
}

/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

static int digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 99;
}

/*
 * Read digits of `radix` with `_` separators into lx->buf (without the
 * separators). Reports a misplaced `_` or a digit too large for the radix.
 */
static void read_digits(Lexer *lx, int radix, const char *radix_name) {
    bool prev_digit = false;
    bool reported = false;
    for (;;) {
        char c = peek(lx);
        if (c == '_') {
            PlumSpan at = here(lx);
            at.length = 1;
            advance(lx);
            if ((!prev_digit || digit_value(peek(lx)) >= radix) && !reported) {
                plum_diag_error(lx->diags, at, "'_' must be between two digits");
                reported = true;
            }
            prev_digit = false;
            continue;
        }
        if (!is_alnum(c)) {
            return;
        }
        int d = digit_value(c);
        if (d >= radix) {
            if (radix == 10 && (c == 'e' || c == 'E')) {
                return; /* an exponent; the caller reads it */
            }
            /* Report the first stray character once and skip the rest of
             * the word, so `12abc` is one error, not three. */
            PlumSpan at = here(lx);
            at.length = 1;
            if (radix != 10 && d < 16) {
                plum_diag_error(lx->diags, at, "'%c' is not a digit in a %s number", c, radix_name);
            } else {
                plum_diag_error(lx->diags, at, "unexpected '%c' after a number", c);
            }
            while (is_alnum(peek(lx))) {
                advance(lx);
            }
            return;
        }
        plum_array_push(&lx->buf, c);
        prev_digit = true;
        advance(lx);
    }
}

static void lex_number(Lexer *lx) {
    PlumSpan start = here(lx);
    lx->buf.len = 0;
    size_t errors_before = lx->diags->errors;

    int radix = 10;
    const char *radix_name = "decimal";
    char p1 = peek_at(lx, 1);
    if (peek(lx) == '0' &&
        (p1 == 'x' || p1 == 'X' || p1 == 'o' || p1 == 'O' || p1 == 'b' || p1 == 'B')) {
        radix = (p1 == 'x' || p1 == 'X') ? 16 : (p1 == 'o' || p1 == 'O') ? 8 : 2;
        radix_name = radix == 16 ? "hexadecimal" : radix == 8 ? "octal" : "binary";
        advance(lx);
        advance(lx);
        read_digits(lx, radix, radix_name);
        if (lx->buf.len == 0 && lx->diags->errors == errors_before) {
            plum_diag_error(lx->diags, span_from(lx, start), "%s number has no digits", radix_name);
        }
    } else {
        read_digits(lx, 10, radix_name);
    }

    bool is_float = false;
    if (radix == 10) {
        /* A fraction needs a digit after the point: `1..5` is a range. */
        if (peek(lx) == '.' && is_digit(peek_at(lx, 1))) {
            is_float = true;
            plum_array_push(&lx->buf, '.');
            advance(lx);
            read_digits(lx, 10, radix_name);
        }
        if (peek(lx) == 'e' || peek(lx) == 'E') {
            is_float = true;
            plum_array_push(&lx->buf, 'e');
            advance(lx);
            if (peek(lx) == '+' || peek(lx) == '-') {
                plum_array_push(&lx->buf, peek(lx));
                advance(lx);
            }
            size_t before = lx->buf.len;
            read_digits(lx, 10, radix_name);
            if (lx->buf.len == before) {
                plum_diag_error(lx->diags, span_from(lx, start), "exponent has no digits");
            }
        }
    }

    PlumSpan span = span_from(lx, start);
    plum_array_push(&lx->buf, '\0');

    if (lx->diags->errors != errors_before) {
        emit(lx, PLUM_TOK_ERROR, span);
        return;
    }

    if (is_float) {
        double value = strtod(lx->buf.items, NULL);
        if (isinf(value)) {
            plum_diag_error(lx->diags, span, "float is too large");
            emit(lx, PLUM_TOK_ERROR, span);
            return;
        }
        emit(lx, PLUM_TOK_FLOAT, span)->as.f = value;
        return;
    }

    if (radix == 10 && lx->buf.items[0] == '0' && lx->buf.items[1] != '\0') {
        const char *digits = lx->buf.items;
        while (digits[0] == '0' && digits[1] != '\0') {
            digits++;
        }
        plum_diag_error(lx->diags, span,
                        "a number can't start with 0; write %s, or 0o%s for an octal number",
                        digits, digits);
        emit(lx, PLUM_TOK_ERROR, span);
        return;
    }

    uint64_t value = 0;
    for (const char *d = lx->buf.items; *d != '\0'; d++) {
        uint64_t digit = (uint64_t)digit_value(*d);
        if (value > (UINT64_C(0x7FFFFFFFFFFFFFFF) - digit) / (uint64_t)radix) {
            plum_diag_error(lx->diags, span,
                            "integer is too large; the largest int is 9223372036854775807");
            emit(lx, PLUM_TOK_ERROR, span);
            return;
        }
        value = value * (uint64_t)radix + digit;
    }
    emit(lx, PLUM_TOK_INT, span)->as.i = (int64_t)value;
}

/* ------------------------------------------------------------------ */
/* Names and keywords                                                  */
/* ------------------------------------------------------------------ */

static const struct {
    const char *word;
    PlumTokenKind kind;
} keywords[] = {
    {"and", PLUM_TOK_AND},
    {"as", PLUM_TOK_AS},
    {"bad", PLUM_TOK_BAD},
    {"branch", PLUM_TOK_BRANCH},
    {"break", PLUM_TOK_BREAK},
    {"catch", PLUM_TOK_CATCH},
    {"continue", PLUM_TOK_CONTINUE},
    {"else", PLUM_TOK_ELSE},
    {"false", PLUM_TOK_BAD},
    {"finally", PLUM_TOK_FINALLY},
    {"for", PLUM_TOK_FOR},
    {"good", PLUM_TOK_GOOD},
    {"if", PLUM_TOK_IF},
    {"in", PLUM_TOK_IN},
    {"is", PLUM_TOK_IS},
    {"kind", PLUM_TOK_KIND},
    {"match", PLUM_TOK_MATCH},
    {"none", PLUM_TOK_NONE},
    {"not", PLUM_TOK_NOT},
    {"or", PLUM_TOK_OR},
    {"plant", PLUM_TOK_PLANT},
    {"return", PLUM_TOK_RETURN},
    {"root", PLUM_TOK_ROOT},
    {"seed", PLUM_TOK_SEED},
    {"self", PLUM_TOK_SELF},
    {"spawn", PLUM_TOK_SPAWN},
    {"super", PLUM_TOK_SUPER},
    {"throw", PLUM_TOK_THROW},
    {"trait", PLUM_TOK_TRAIT},
    {"tree", PLUM_TOK_TREE},
    {"true", PLUM_TOK_GOOD},
    {"try", PLUM_TOK_TRY},
    {"weak", PLUM_TOK_WEAK},
    {"while", PLUM_TOK_WHILE},
};

static void lex_name(Lexer *lx) {
    PlumSpan start = here(lx);
    while (is_alnum(peek(lx))) {
        advance(lx);
    }
    PlumSpan span = span_from(lx, start);
    const char *word = lx->text + span.offset;
    size_t len = span.length;

    if (len == 1 && word[0] == '_') {
        emit(lx, PLUM_TOK_UNDERSCORE, span);
        return;
    }
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strlen(keywords[i].word) == len && memcmp(keywords[i].word, word, len) == 0) {
            emit(lx, keywords[i].kind, span);
            return;
        }
    }
    emit(lx, PLUM_TOK_IDENT, span)->as.name = plum_intern(lx->names, word, len);
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

static void buf_append(Lexer *lx, const char *s, size_t n) {
    plum_array_reserve(&lx->buf, n);
    memcpy(lx->buf.items + lx->buf.len, s, n);
    lx->buf.len += n;
}

/* Read an escape after the backslash; append its text to lx->buf. */
static void read_escape(Lexer *lx, PlumSpan backslash) {
    char c = peek(lx);
    const char *simple = NULL;
    switch (c) {
    case 'n':
        simple = "\n";
        break;
    case 't':
        simple = "\t";
        break;
    case 'r':
        simple = "\r";
        break;
    case '\\':
        simple = "\\";
        break;
    case '"':
        simple = "\"";
        break;
    case '{':
        simple = "{";
        break;
    case '}':
        simple = "}";
        break;
    case '0':
        advance(lx);
        plum_array_push(&lx->buf, '\0');
        return;
    default:
        break;
    }
    if (simple != NULL) {
        advance(lx);
        buf_append(lx, simple, 1);
        return;
    }

    if (c == 'u') {
        advance(lx);
        if (peek(lx) != '{') {
            plum_diag_error(lx->diags, span_from(lx, backslash),
                            "\\u needs a code point in braces, like \\u{1F351}");
            return;
        }
        advance(lx);
        uint32_t cp = 0;
        int digits = 0;
        while (digit_value(peek(lx)) < 16) {
            if (digits < 7) {
                cp = cp * 16 + (uint32_t)digit_value(peek(lx));
            }
            digits++;
            advance(lx);
        }
        if (peek(lx) != '}' || digits == 0 || digits > 6) {
            while (!at_end(lx) && peek(lx) != '}' && peek(lx) != '"' && peek(lx) != '\n') {
                advance(lx);
            }
            if (peek(lx) == '}') {
                advance(lx);
            }
            plum_diag_error(lx->diags, span_from(lx, backslash),
                            "\\u{...} needs 1 to 6 hexadecimal digits");
            return;
        }
        advance(lx);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            plum_diag_error(lx->diags, span_from(lx, backslash),
                            "\\u{%X} is not a Unicode character", (unsigned)cp);
            return;
        }
        char enc[4];
        buf_append(lx, enc, utf8_encode(cp, enc));
        return;
    }

    if (at_end(lx) || c == '\n') {
        return; /* the unclosed string is reported by the caller */
    }
    advance(lx);
    plum_diag_error(lx->diags, span_from(lx, backslash), "unknown escape '\\%c'%s", c,
                    c == '\'' ? "; a single quote needs no escape" : "");
}

/*
 * For a """ string starting after the opening quotes at `from`, find the
 * indentation of the closing """: the spaces and tabs before it, when
 * nothing else precedes it on its line. Returns 0 if there is no closing
 * """ or text precedes it.
 */
static size_t triple_indent(const Lexer *lx, size_t from) {
    for (size_t i = from; i + 2 < lx->length; i++) {
        char c = lx->text[i];
        if (c == '\\') {
            i++;
            continue;
        }
        if (c == '"' && lx->text[i + 1] == '"' && lx->text[i + 2] == '"') {
            size_t j = i;
            while (j > from && (lx->text[j - 1] == ' ' || lx->text[j - 1] == '\t')) {
                j--;
            }
            if (j > from && lx->text[j - 1] == '\n') {
                return i - j;
            }
            return 0;
        }
    }
    return 0;
}

/*
 * In a """ string at the start of a line: skip up to `indent` spaces and
 * tabs. A line with text that is indented less is an error.
 */
static void skip_indent(Lexer *lx, size_t indent) {
    PlumSpan start = here(lx);
    size_t n = 0;
    while (n < indent && (peek(lx) == ' ' || peek(lx) == '\t')) {
        advance(lx);
        n++;
    }
    bool blank = peek(lx) == '\n' || (peek(lx) == '\r' && peek_at(lx, 1) == '\n');
    bool closing = peek(lx) == '"' && peek_at(lx, 1) == '"' && peek_at(lx, 2) == '"';
    if (n < indent && !blank && !closing && !at_end(lx)) {
        plum_diag_error(lx->diags, span_from(lx, start),
                        "this line is indented less than the closing \"\"\"");
    }
}

/*
 * Read string text up to the closing quote(s) or a `{`, and emit one token:
 * STRING or STRING_HEAD when `first`, else STRING_MID or STRING_TAIL. On
 * `{`, an interpolation is opened and lexing goes back to normal tokens.
 */
static void lex_string_part(Lexer *lx, bool first, bool triple, size_t indent, PlumSpan quote,
                            PlumSpan start) {
    /* `start` is the '"' or '}' this part begins with; tokens span from it. */
    start.length = 0;
    lx->buf.len = 0;
    bool line_start = false;

    if (first && triple) {
        /* The first newline after the opening """ is dropped. */
        if (peek(lx) == '\r' && peek_at(lx, 1) == '\n') {
            advance(lx);
        }
        if (peek(lx) == '\n') {
            advance(lx);
            line_start = true;
        }
    }

    for (;;) {
        if (line_start) {
            skip_indent(lx, indent);
            line_start = false;
        }
        char c = peek(lx);

        if (at_end(lx) || (c == '\n' && !triple)) {
            plum_diag_error(lx->diags, quote, "string is not closed; add a '\"'%s",
                            triple ? "\"\" at its end" : " before the end of the line");
            PlumSpan span = span_from(lx, start);
            PlumToken *tok = emit(lx, first ? PLUM_TOK_STRING : PLUM_TOK_STRING_TAIL, span);
            tok->as.str.chars =
                plum_arena_strndup(lx->arena, lx->buf.items ? lx->buf.items : "", lx->buf.len);
            tok->as.str.len = lx->buf.len;
            return;
        }

        bool closes = c == '"' && (!triple || (peek_at(lx, 1) == '"' && peek_at(lx, 2) == '"'));
        if (closes || c == '{') {
            PlumSpan span = span_from(lx, start);
            PlumTokenKind kind;
            if (closes) {
                kind = first ? PLUM_TOK_STRING : PLUM_TOK_STRING_TAIL;
            } else {
                kind = first ? PLUM_TOK_STRING_HEAD : PLUM_TOK_STRING_MID;
            }
            PlumToken *tok = emit(lx, kind, span);
            tok->as.str.chars =
                plum_arena_strndup(lx->arena, lx->buf.items ? lx->buf.items : "", lx->buf.len);
            tok->as.str.len = lx->buf.len;

            if (closes) {
                for (int i = 0; i < (triple ? 3 : 1); i++) {
                    advance(lx);
                }
                /* The token covers the string up to and including its end. */
                plum_array_last(lx->out).span = span_from(lx, plum_array_last(lx->out).span);
            } else {
                Open open = {'I', triple, indent, quote};
                advance(lx);
                plum_array_last(lx->out).span = span_from(lx, plum_array_last(lx->out).span);
                plum_array_push(&lx->open, open);
            }
            return;
        }

        if (c == '}') {
            PlumSpan at = here(lx);
            at.length = 1;
            advance(lx);
            plum_diag_error(lx->diags, at, "'}' in a string must be written '\\}'");
            continue;
        }

        if (c == '\\') {
            PlumSpan backslash = here(lx);
            advance(lx);
            read_escape(lx, backslash);
            continue;
        }

        if (c == '\r' && peek_at(lx, 1) == '\n') {
            advance(lx); /* CRLF in a """ string is a newline */
            continue;
        }

        size_t from = lx->pos;
        if (!advance(lx)) {
            continue; /* invalid UTF-8, already reported */
        }
        buf_append(lx, lx->text + from, lx->pos - from);
        if (c == '\n') {
            line_start = true;
        }
    }
}

static void lex_string(Lexer *lx) {
    PlumSpan quote = here(lx);
    bool triple = peek_at(lx, 1) == '"' && peek_at(lx, 2) == '"';
    for (int i = 0; i < (triple ? 3 : 1); i++) {
        advance(lx);
    }
    PlumSpan start = quote;
    quote = span_from(lx, quote);
    size_t indent = triple ? triple_indent(lx, lx->pos) : 0;
    lex_string_part(lx, true, triple, indent, quote, start);
}

/* A `:` at the top of an interpolation starts a format: "{x:.2}". */
static void lex_format(Lexer *lx) {
    advance(lx); /* ':' */
    PlumSpan start = here(lx);
    while (!at_end(lx) && peek(lx) != '}' && peek(lx) != '"' && peek(lx) != '\n') {
        advance(lx);
    }
    PlumSpan span = span_from(lx, start);
    if (span.length == 0) {
        plum_diag_error(lx->diags, span, "empty format after ':'");
    }
    PlumToken *tok = emit(lx, PLUM_TOK_FORMAT, span);
    tok->as.str.chars = plum_arena_strndup(lx->arena, lx->text + span.offset, span.length);
    tok->as.str.len = span.length;
}

/* ------------------------------------------------------------------ */
/* Brackets                                                            */
/* ------------------------------------------------------------------ */

static void open_bracket(Lexer *lx, char kind, PlumTokenKind tok) {
    PlumSpan at = here(lx);
    advance(lx);
    at = span_from(lx, at);
    emit(lx, tok, at);
    Open open = {kind, false, 0, at};
    plum_array_push(&lx->open, open);
}

static char matching(char close) {
    return close == ')' ? '(' : close == ']' ? '[' : '{';
}

static void close_bracket(Lexer *lx, char close, PlumTokenKind tok) {
    PlumSpan at = here(lx);
    advance(lx);
    at = span_from(lx, at);

    char want = matching(close);
    if (open_top(lx) == want) {
        lx->open.len--;
    } else {
        /* Close what this bracket matches further out, if anything. */
        size_t i = lx->open.len;
        while (i > 0 && lx->open.items[i - 1].kind != want && lx->open.items[i - 1].kind != 'I') {
            i--;
        }
        if (i > 0 && lx->open.items[i - 1].kind == want) {
            Open *inner = &lx->open.items[lx->open.len - 1];
            plum_diag_error(lx->diags, at, "'%c' doesn't match the '%c' opened at %u:%u", close,
                            inner->kind, (unsigned)inner->span.line, (unsigned)inner->span.column);
            lx->open.len = i - 1;
        } else if (open_top(lx) == '(' || open_top(lx) == '[' || open_top(lx) == '{') {
            /* Nothing further out matches either: most likely the closer is
             * a typo for the innermost bracket's, so treat it as closing it. */
            Open *inner = &plum_array_last(&lx->open);
            plum_diag_error(lx->diags, at, "'%c' doesn't match the '%c' opened at %u:%u", close,
                            inner->kind, (unsigned)inner->span.line, (unsigned)inner->span.column);
            lx->open.len--;
        } else {
            plum_diag_error(lx->diags, at, "'%c' has no matching '%c'", close, want);
        }
    }
    emit(lx, tok, at);
}

/* ------------------------------------------------------------------ */
/* Operators                                                           */
/* ------------------------------------------------------------------ */

static const struct {
    const char *text;
    PlumTokenKind kind;
} operators[] = {
    /* Longest first, so the first match is the longest. */
    {"//=", PLUM_TOK_SLASH_SLASH_EQ},
    {"**=", PLUM_TOK_STAR_STAR_EQ},
    {"<<=", PLUM_TOK_LESS_LESS_EQ},
    {">>=", PLUM_TOK_GREATER_GREATER_EQ},
    {"..=", PLUM_TOK_DOT_DOT_EQ},
    {"..", PLUM_TOK_DOT_DOT},
    {"->", PLUM_TOK_ARROW},
    {"=>", PLUM_TOK_FAT_ARROW},
    {"?.", PLUM_TOK_QUESTION_DOT},
    {"??", PLUM_TOK_QUESTION_QUESTION},
    {"//", PLUM_TOK_SLASH_SLASH},
    {"**", PLUM_TOK_STAR_STAR},
    {"<<", PLUM_TOK_LESS_LESS},
    {">>", PLUM_TOK_GREATER_GREATER},
    {"==", PLUM_TOK_EQ_EQ},
    {"!=", PLUM_TOK_BANG_EQ},
    {"<=", PLUM_TOK_LESS_EQ},
    {">=", PLUM_TOK_GREATER_EQ},
    {"+=", PLUM_TOK_PLUS_EQ},
    {"-=", PLUM_TOK_MINUS_EQ},
    {"*=", PLUM_TOK_STAR_EQ},
    {"/=", PLUM_TOK_SLASH_EQ},
    {"%=", PLUM_TOK_PERCENT_EQ},
    {"&=", PLUM_TOK_AMP_EQ},
    {"|=", PLUM_TOK_PIPE_EQ},
    {"^=", PLUM_TOK_CARET_EQ},
    {",", PLUM_TOK_COMMA},
    {":", PLUM_TOK_COLON},
    {".", PLUM_TOK_DOT},
    {"?", PLUM_TOK_QUESTION},
    {"+", PLUM_TOK_PLUS},
    {"-", PLUM_TOK_MINUS},
    {"*", PLUM_TOK_STAR},
    {"/", PLUM_TOK_SLASH},
    {"%", PLUM_TOK_PERCENT},
    {"&", PLUM_TOK_AMP},
    {"|", PLUM_TOK_PIPE},
    {"^", PLUM_TOK_CARET},
    {"~", PLUM_TOK_TILDE},
    {"<", PLUM_TOK_LESS},
    {">", PLUM_TOK_GREATER},
    {"=", PLUM_TOK_EQ},
};

/* Spellings from other languages, with what to write instead. */
static const struct {
    const char *text;
    const char *hint;
} foreign[] = {
    {"&&", "'&&' is written 'and' in Plumlang"},
    {"||", "'||' is written 'or' in Plumlang"},
    {"++", "Plumlang has no '++'; write '+= 1'"},
    {"--", "Plumlang has no '--'; write '-= 1'"},
};

static bool lex_operator(Lexer *lx) {
    PlumSpan start = here(lx);
    const char *s = lx->text + lx->pos;
    size_t left = lx->length - lx->pos;

    for (size_t i = 0; i < sizeof foreign / sizeof foreign[0]; i++) {
        if (left >= 2 && memcmp(s, foreign[i].text, 2) == 0) {
            advance(lx);
            advance(lx);
            PlumSpan span = span_from(lx, start);
            plum_diag_error(lx->diags, span, "%s", foreign[i].hint);
            emit(lx, PLUM_TOK_ERROR, span);
            return true;
        }
    }
    for (size_t i = 0; i < sizeof operators / sizeof operators[0]; i++) {
        size_t n = strlen(operators[i].text);
        if (left >= n && memcmp(s, operators[i].text, n) == 0) {
            for (size_t k = 0; k < n; k++) {
                advance(lx);
            }
            emit(lx, operators[i].kind, span_from(lx, start));
            return true;
        }
    }
    return false;
}

static void unexpected(Lexer *lx) {
    PlumSpan start = here(lx);
    char c = peek(lx);
    size_t from = lx->pos;
    if (!advance(lx)) {
        emit(lx, PLUM_TOK_ERROR, span_from(lx, start));
        return; /* invalid UTF-8, already reported */
    }
    PlumSpan span = span_from(lx, start);
    switch (c) {
    case ';':
        plum_diag_error(lx->diags, span,
                        "Plumlang doesn't use ';'; end the statement with a new line");
        break;
    case '\'': {
        /* Take everything up to the closing ' on this line as the string,
         * so parsing can carry on, and show how to write it. */
        size_t text_from = lx->pos;
        while (!at_end(lx) && peek(lx) != '\'' && peek(lx) != '\n') {
            advance(lx);
        }
        size_t text_len = lx->pos - text_from;
        if (peek(lx) == '\'') {
            advance(lx);
        }
        span = span_from(lx, start);
        plum_diag_error(lx->diags, span, "strings use double quotes: \"%.*s\"", (int)text_len,
                        lx->text + text_from);
        PlumToken *tok = emit(lx, PLUM_TOK_STRING, span);
        tok->as.str.chars = plum_arena_strndup(lx->arena, lx->text + text_from, text_len);
        tok->as.str.len = text_len;
        return;
    }
    case '!':
        plum_diag_error(lx->diags, span, "'!' is written 'not' in Plumlang");
        break;
    default:
        plum_diag_error(lx->diags, span, "unexpected character '%.*s'", (int)(lx->pos - from),
                        lx->text + from);
        break;
    }
    emit(lx, PLUM_TOK_ERROR, span);
}

/* ------------------------------------------------------------------ */
/* The main loop                                                       */
/* ------------------------------------------------------------------ */

void plum_lex(const PlumSource *source, PlumArena *arena, PlumInterner *names, PlumDiagList *diags,
              PlumTokenList *out) {
    Lexer lx;
    memset(&lx, 0, sizeof lx);
    lx.source = source;
    lx.text = source->text;
    lx.length = source->length;
    lx.line = 1;
    lx.column = 1;
    lx.arena = arena;
    lx.names = names;
    lx.diags = diags;
    lx.out = out;

    if (lx.length >= 3 && memcmp(lx.text, "\xEF\xBB\xBF", 3) == 0) {
        lx.pos = 3; /* byte order mark */
    }

    while (!at_end(&lx)) {
        char c = peek(&lx);
        char top = open_top(&lx);

        if (c == ' ' || c == '\t' || c == '\r') {
            advance(&lx);
        } else if (c == '\n') {
            newline(&lx);
        } else if (c == '#') {
            while (!at_end(&lx) && peek(&lx) != '\n') {
                advance(&lx);
            }
        } else if (is_digit(c)) {
            lex_number(&lx);
        } else if (is_alpha(c)) {
            lex_name(&lx);
        } else if (c == '"') {
            lex_string(&lx);
        } else if (c == '}' && top == 'I') {
            Open interp = plum_array_pop(&lx.open);
            PlumSpan brace = here(&lx);
            advance(&lx);
            lex_string_part(&lx, false, interp.triple, interp.indent, interp.span, brace);
        } else if (c == ':' && top == 'I') {
            lex_format(&lx);
        } else if (c == '(') {
            open_bracket(&lx, '(', PLUM_TOK_LPAREN);
        } else if (c == '[') {
            open_bracket(&lx, '[', PLUM_TOK_LBRACKET);
        } else if (c == '{') {
            open_bracket(&lx, '{', PLUM_TOK_LBRACE);
        } else if (c == ')') {
            close_bracket(&lx, ')', PLUM_TOK_RPAREN);
        } else if (c == ']') {
            close_bracket(&lx, ']', PLUM_TOK_RBRACKET);
        } else if (c == '}') {
            close_bracket(&lx, '}', PLUM_TOK_RBRACE);
        } else if (!lex_operator(&lx)) {
            unexpected(&lx);
        }
    }

    /* Whatever is still open is never closed. */
    for (size_t i = lx.open.len; i > 0; i--) {
        Open *o = &lx.open.items[i - 1];
        if (o->kind == 'I') {
            plum_diag_error(diags, o->span, "string is not closed; add a '\"'");
        } else {
            plum_diag_error(diags, o->span, "'%c' is never closed", o->kind);
        }
    }

    PlumSpan end = here(&lx);
    if (out->len > 0 && last_kind(&lx) != PLUM_TOK_NEWLINE) {
        emit(&lx, PLUM_TOK_NEWLINE, end);
    }
    emit(&lx, PLUM_TOK_EOF, end);

    plum_array_free(&lx.open);
    plum_array_free(&lx.buf);
}
