/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/vm.h"

#include "compiler/chunk.h"
#include "util/alloc.h"
#include "util/fmt.h"
#include "vm/methods.h"
#include "vm/text.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct Frame {
    PlumFunction *fn;
    const uint8_t *ip;
    size_t base; /* index of slot 0 in the stack; the function itself sits at base - 1 */
} Frame;

typedef struct VM {
    PlumValue *stack;
    size_t top;
    size_t capacity;
    Frame frames[PLUM_MAX_FRAMES];
    int frame_count;
    PlumValue *globals;
    int global_count;
    const PlumSource *source;
    FILE *out;
    FILE *err;
    int exit_code;
    bool stopped; /* exit() was called */
} VM;

/* ------------------------------------------------------------------ */
/* The stack                                                           */
/* ------------------------------------------------------------------ */

static void grow(VM *vm, size_t need) {
    if (need <= vm->capacity) {
        return;
    }
    size_t cap = vm->capacity < 256 ? 256 : vm->capacity;
    while (cap < need) {
        cap = plum_mul_size(cap, 2);
    }
    vm->stack = plum_realloc_array(vm->stack, cap, sizeof(PlumValue));
    vm->capacity = cap;
}

static inline void push(VM *vm, PlumValue v) {
    if (vm->top == vm->capacity) {
        grow(vm, vm->top + 1);
    }
    vm->stack[vm->top++] = v;
}

static inline PlumValue pop(VM *vm) {
    return vm->stack[--vm->top];
}

static inline PlumValue *peek(VM *vm, size_t from_top) {
    return &vm->stack[vm->top - 1 - from_top];
}

/* ------------------------------------------------------------------ */
/* Runtime errors                                                      */
/* ------------------------------------------------------------------ */

static uint32_t frame_line(const Frame *f) {
    size_t offset = (size_t)(f->ip - f->fn->chunk->code.items);
    return plum_chunk_line(f->fn->chunk, offset > 0 ? offset - 1 : 0);
}

/*
 * Report a runtime error: where it happened, then the calls that led
 * there, innermost first.
 */
PLUM_PRINTF(2, 3)
static void runtime_error(VM *vm, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const Frame *top = &vm->frames[vm->frame_count - 1];
    fprintf(vm->err, "%s:%u: runtime error: ", vm->source->path, (unsigned)frame_line(top));
    vfprintf(vm->err, fmt, args);
    fputc('\n', vm->err);
    va_end(args);
    /* Innermost calls first; a deep recursion shows its two ends. */
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        int from_top = vm->frame_count - 1 - i;
        if (from_top == 10 && i > 2) {
            /* frames i down to 2 are skipped; 1 and 0 are shown */
            fprintf(vm->err, "  ... %d more calls\n", i - 1);
            i = 1;
        }
        const Frame *f = &vm->frames[i];
        fprintf(vm->err, "  in %s, line %u\n", f->fn->name, (unsigned)frame_line(f));
    }
    vm->exit_code = 1;
}

/* ------------------------------------------------------------------ */
/* Integer arithmetic: overflow is an error, never wrap-around         */
/* ------------------------------------------------------------------ */

static bool add_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return true;
    }
    *out = a + b;
    return false;
}

static bool sub_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
        return true;
    }
    *out = a - b;
    return false;
}

static bool mul_overflows(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return false;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return true;
    }
    if (a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
              : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b)) {
        return true;
    }
    *out = a * b;
    return false;
}

static bool pow_overflows(int64_t base, int64_t exp, int64_t *out) {
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) {
            if (mul_overflows(result, base, &result)) {
                return true;
            }
        }
        exp >>= 1;
        if (exp > 0 && mul_overflows(base, base, &base)) {
            return true;
        }
    }
    *out = result;
    return false;
}

/* Floor division and a remainder with the sign of the divisor (spec 4.4). */
static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int64_t floor_mod(int64_t a, int64_t b) {
    if (b == -1) {
        return 0; /* INT64_MIN % -1 is undefined in C */
    }
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        r += b;
    }
    return r;
}

static double float_mod(double a, double b) {
    double r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) {
        r += b;
    }
    return r;
}

static int64_t shift_right(int64_t a, int64_t n) {
    /* Arithmetic, without relying on how C shifts negative numbers. */
    return a < 0 ? ~(~a >> n) : a >> n;
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

static int compare_strings(const PlumString *a, const PlumString *b) {
    size_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->chars, b->chars, n); /* UTF-8 byte order is code point order */
    if (c != 0) {
        return c;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

static PlumString *concat(const PlumString *a, const PlumString *b) {
    PlumString *s = plum_string_alloc(plum_add_size(a->len, b->len));
    memcpy(s->chars, a->chars, a->len);
    memcpy(s->chars + a->len, b->chars, b->len);
    return s;
}

/* ------------------------------------------------------------------ */
/* Built-in functions                                                  */
/* ------------------------------------------------------------------ */

/* Parse a whole string as an int or float; false if it isn't one. */
static bool parse_int(const PlumString *s, int64_t *out) {
    const char *p = s->chars;
    size_t n = s->len;
    /* Surrounding spaces are fine, as when the text came from ask(). */
    while (n > 0 && (*p == ' ' || *p == '\t')) {
        p++;
        n--;
    }
    while (n > 0 && strchr(" \t\r\n", p[n - 1]) != NULL) {
        n--;
    }
    bool negative = false;
    if (n > 0 && (*p == '-' || *p == '+')) {
        negative = *p == '-';
        p++;
        n--;
    }
    if (n == 0) {
        return false;
    }
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '_' && i > 0 && i + 1 < n && p[i - 1] != '_') {
            continue; /* 1_000 */
        }
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        uint64_t d = (uint64_t)(p[i] - '0');
        if (v > (limit - d) / 10) {
            return false; /* too large for an int */
        }
        v = v * 10 + d;
    }
    *out = negative ? (int64_t)(0 - v) : (int64_t)v;
    return true;
}

static bool parse_float(const PlumString *s, double *out) {
    char *end = NULL;
    const char *p = s->chars;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    double v = strtod(p, &end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        end++;
    }
    if (*end != '\0' || end == p) {
        return false;
    }
    *out = v;
    return true;
}

static const char *type_name(PlumValue v) {
    switch (v.type) {
    case PLUM_VAL_NONE:
        return "none";
    case PLUM_VAL_BOOL:
        return "bool";
    case PLUM_VAL_INT:
        return "int";
    case PLUM_VAL_FLOAT:
        return "float";
    case PLUM_VAL_OBJ:
        switch ((PlumObjType)v.as.obj->type) {
        case PLUM_OBJ_STRING:
            return "str";
        case PLUM_OBJ_FUNCTION:
            return "branch";
        case PLUM_OBJ_LIST:
            return "list";
        }
    }
    return "?";
}

static PlumValue str_value(PlumString *s) {
    return plum_obj(&s->obj);
}

/* A line from standard input without its newline, or none at the end. */
static PlumValue read_line(void) {
    PLUM_ARRAY(char) line = {0};
    int ch;
    bool any = false;
    while ((ch = getchar()) != EOF) {
        any = true;
        if (ch == '\n') {
            break;
        }
        plum_array_push(&line, (char)ch);
    }
    if (!any) {
        return plum_none();
    }
    size_t n = line.len;
    if (n > 0 && line.items[n - 1] == '\r') {
        n--;
    }
    PlumString *s = plum_string_new(line.items ? line.items : "", n);
    plum_array_free(&line);
    return str_value(s);
}

/*
 * Run built-in `id` on the top `argc` values. Pushes its result and returns
 * true, or reports a runtime error and returns false.
 */
static bool call_builtin(VM *vm, PlumBuiltin id, int argc) {
    PlumValue *args = &vm->stack[vm->top - (size_t)argc];
    PlumValue result = plum_none();
    char error[256];

    switch (id) {
    case PLUM_BUILTIN_DROP:
        for (int i = 0; i < argc; i++) {
            if (i > 0) {
                fputc(' ', vm->out);
            }
            plum_print_value(vm->out, args[i]);
        }
        fputc('\n', vm->out);
        break;
    case PLUM_BUILTIN_LEN:
        if (plum_is_obj(args[0], PLUM_OBJ_LIST)) {
            result = plum_int((int64_t)plum_as_list(args[0])->len);
        } else {
            PlumString *s = plum_as_string(args[0]);
            result = plum_int((int64_t)plum_utf8_length(s->chars, s->len));
        }
        break;
    case PLUM_BUILTIN_STR: {
        PlumString *s = plum_value_text(args[0], NULL, error, sizeof error);
        result = str_value(s);
        break;
    }
    case PLUM_BUILTIN_INT: {
        PlumValue v = args[0];
        if (v.type == PLUM_VAL_INT) {
            result = v;
        } else if (v.type == PLUM_VAL_FLOAT) {
            double f = v.as.f;
            if (isnan(f) || f >= 9223372036854775808.0 || f < -9223372036854775808.0) {
                char buf[PLUM_FLOAT_BUF];
                runtime_error(vm, "int(%s) doesn't fit in an int", plum_format_float(f, buf));
                return false;
            }
            result = plum_int((int64_t)f); /* truncates toward zero */
        } else {
            int64_t i;
            if (!parse_int(plum_as_string(v), &i)) {
                runtime_error(vm, "int(\"%s\"): not a whole number", plum_as_string(v)->chars);
                return false;
            }
            result = plum_int(i);
        }
        break;
    }
    case PLUM_BUILTIN_FLOAT: {
        PlumValue v = args[0];
        if (v.type == PLUM_VAL_FLOAT) {
            result = v;
        } else if (v.type == PLUM_VAL_INT) {
            result = plum_float((double)v.as.i);
        } else {
            double f;
            if (!parse_float(plum_as_string(v), &f)) {
                runtime_error(vm, "float(\"%s\"): not a number", plum_as_string(v)->chars);
                return false;
            }
            result = plum_float(f);
        }
        break;
    }
    case PLUM_BUILTIN_BYTE:
        if (args[0].as.i < 0 || args[0].as.i > 255) {
            runtime_error(vm, "byte(%lld) is out of range; a byte is 0 to 255",
                          (long long)args[0].as.i);
            return false;
        }
        result = args[0];
        break;
    case PLUM_BUILTIN_TYPE_OF: {
        const char *name = type_name(args[0]);
        result = str_value(plum_string_new(name, strlen(name)));
        break;
    }
    case PLUM_BUILTIN_ASSERT:
        if (!args[0].as.b) {
            if (argc > 1 && plum_as_string(args[1])->len > 0) {
                runtime_error(vm, "assertion failed: %s", plum_as_string(args[1])->chars);
            } else {
                runtime_error(vm, "assertion failed");
            }
            return false;
        }
        break;
    case PLUM_BUILTIN_EXIT:
        vm->exit_code = argc > 0 ? (int)args[0].as.i : 0;
        vm->stopped = true;
        break;
    case PLUM_BUILTIN_ASK:
        if (argc > 0) {
            plum_print_value(vm->out, args[0]);
            fflush(vm->out);
        }
        result = read_line();
        break;
    case PLUM_BUILTIN_COUNT:
        break;
    }

    for (int i = 0; i < argc; i++) {
        plum_release(pop(vm));
    }
    push(vm, result);
    return true;
}

/* ------------------------------------------------------------------ */
/* Lists                                                               */
/* ------------------------------------------------------------------ */

/* Is `i` a position in a list of `len` items? Reports the error if not. */
static bool check_index(VM *vm, int64_t i, size_t len) {
    if (i < 0 || (uint64_t)i >= len) {
        if (len == 0) {
            runtime_error(vm, "index %lld is out of range: the list is empty", (long long)i);
        } else {
            runtime_error(vm, "index %lld is out of range: the list has %zu item%s (0 to %zu)",
                          (long long)i, len, len == 1 ? "" : "s", len - 1);
        }
        return false;
    }
    return true;
}

static PlumList *slice_list(const PlumList *list, size_t from, size_t to) {
    PlumList *out = plum_list_new(to - from);
    for (size_t i = from; i < to; i++) {
        out->items[out->len++] = plum_retain(list->items[i]);
    }
    return out;
}

static PlumList *concat_lists(const PlumList *a, const PlumList *b) {
    PlumList *out = plum_list_new(plum_add_size(a->len, b->len));
    for (size_t i = 0; i < a->len; i++) {
        out->items[out->len++] = plum_retain(a->items[i]);
    }
    for (size_t i = 0; i < b->len; i++) {
        out->items[out->len++] = plum_retain(b->items[i]);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

static bool call(VM *vm, PlumFunction *fn, int argc) {
    if (vm->frame_count == PLUM_MAX_FRAMES) {
        runtime_error(vm, "too much recursion: more than %d calls deep", PLUM_MAX_FRAMES);
        return false;
    }
    Frame *f = &vm->frames[vm->frame_count++];
    f->fn = fn;
    f->ip = fn->chunk->code.items;
    f->base = vm->top - (size_t)argc;
    /* The other local slots start as none. */
    grow(vm, f->base + (size_t)fn->slot_count + 1);
    for (int i = argc; i < fn->slot_count; i++) {
        push(vm, plum_none());
    }
    return true;
}

/* Release everything from stack index `to` up. */
static void unwind_to(VM *vm, size_t to) {
    while (vm->top > to) {
        plum_release(pop(vm));
    }
}

/* Read a 16-bit operand and move past it. */
static inline uint16_t read_u16(const uint8_t **ip) {
    uint16_t v = plum_chunk_read_u16(*ip);
    *ip += 2;
    return v;
}

static bool execute(VM *vm) {
    Frame *frame = &vm->frames[vm->frame_count - 1];
    const uint8_t *ip = frame->ip;
    PlumChunk *chunk = frame->fn->chunk;

#define READ_U16() read_u16(&ip)
#define SLOT(n)    (vm->stack[frame->base + (n)])
#define SAVE_IP()  (frame->ip = ip)
#define FAIL(...)                       \
    do {                                \
        SAVE_IP();                      \
        runtime_error(vm, __VA_ARGS__); \
        return false;                   \
    } while (0)
#define INT_BINARY(expr)          \
    do {                          \
        int64_t b = pop(vm).as.i; \
        int64_t a = pop(vm).as.i; \
        push(vm, expr);           \
    } while (0)
#define FLOAT_BINARY(expr)       \
    do {                         \
        double b = pop(vm).as.f; \
        double a = pop(vm).as.f; \
        push(vm, expr);          \
    } while (0)
#define CHECKED(fn, symbol)                                                                    \
    do {                                                                                       \
        int64_t b = pop(vm).as.i;                                                              \
        int64_t a = pop(vm).as.i;                                                              \
        int64_t r;                                                                             \
        if (fn(a, b, &r)) {                                                                    \
            FAIL("integer overflow: %lld %s %lld doesn't fit in an int", (long long)a, symbol, \
                 (long long)b);                                                                \
        }                                                                                      \
        push(vm, plum_int(r));                                                                 \
    } while (0)
#define STR_COMPARE(test)                                              \
    do {                                                               \
        PlumValue b = pop(vm);                                         \
        PlumValue a = pop(vm);                                         \
        int c = compare_strings(plum_as_string(a), plum_as_string(b)); \
        plum_release(a);                                               \
        plum_release(b);                                               \
        push(vm, plum_bool(test));                                     \
    } while (0)

    for (;;) {
        PlumOpcode op = (PlumOpcode)*ip++;
        switch (op) {
        case PLUM_OP_CONST:
            push(vm, plum_retain(chunk->constants.items[READ_U16()]));
            break;
        case PLUM_OP_NONE:
            push(vm, plum_none());
            break;
        case PLUM_OP_TRUE:
            push(vm, plum_bool(true));
            break;
        case PLUM_OP_FALSE:
            push(vm, plum_bool(false));
            break;
        case PLUM_OP_POP:
            plum_release(pop(vm));
            break;
        case PLUM_OP_DUP:
            push(vm, plum_retain(*peek(vm, 0)));
            break;
        case PLUM_OP_GET_LOCAL:
            push(vm, plum_retain(SLOT(READ_U16())));
            break;
        case PLUM_OP_SET_LOCAL: {
            uint16_t n = READ_U16();
            PlumValue v = pop(vm);
            plum_release(SLOT(n));
            SLOT(n) = v;
            break;
        }
        case PLUM_OP_GET_GLOBAL:
            push(vm, plum_retain(vm->globals[READ_U16()]));
            break;
        case PLUM_OP_SET_GLOBAL: {
            uint16_t n = READ_U16();
            PlumValue v = pop(vm);
            plum_release(vm->globals[n]);
            vm->globals[n] = v;
            break;
        }

        case PLUM_OP_ADD_INT:
            CHECKED(add_overflows, "+");
            break;
        case PLUM_OP_SUB_INT:
            CHECKED(sub_overflows, "-");
            break;
        case PLUM_OP_MUL_INT:
            CHECKED(mul_overflows, "*");
            break;
        case PLUM_OP_FLOORDIV_INT:
        case PLUM_OP_MOD_INT: {
            int64_t b = pop(vm).as.i;
            int64_t a = pop(vm).as.i;
            const char *sym = op == PLUM_OP_FLOORDIV_INT ? "//" : "%";
            if (b == 0) {
                FAIL("division by zero: %lld %s 0", (long long)a, sym);
            }
            if (op == PLUM_OP_FLOORDIV_INT && a == INT64_MIN && b == -1) {
                FAIL("integer overflow: %lld // -1 doesn't fit in an int", (long long)a);
            }
            push(vm, plum_int(op == PLUM_OP_FLOORDIV_INT ? floor_div(a, b) : floor_mod(a, b)));
            break;
        }
        case PLUM_OP_POW_INT: {
            int64_t b = pop(vm).as.i;
            int64_t a = pop(vm).as.i;
            int64_t r;
            if (b < 0) {
                FAIL("%lld ** %lld: an int power needs an exponent of 0 or more; use a float "
                     "for fractions",
                     (long long)a, (long long)b);
            }
            if (pow_overflows(a, b, &r)) {
                FAIL("integer overflow: %lld ** %lld doesn't fit in an int", (long long)a,
                     (long long)b);
            }
            push(vm, plum_int(r));
            break;
        }
        case PLUM_OP_NEG_INT: {
            int64_t a = pop(vm).as.i;
            if (a == INT64_MIN) {
                FAIL("integer overflow: -(%lld) doesn't fit in an int", (long long)a);
            }
            push(vm, plum_int(-a));
            break;
        }
        case PLUM_OP_BAND:
            INT_BINARY(plum_int(a & b));
            break;
        case PLUM_OP_BOR:
            INT_BINARY(plum_int(a | b));
            break;
        case PLUM_OP_BXOR:
            INT_BINARY(plum_int(a ^ b));
            break;
        case PLUM_OP_BNOT:
            push(vm, plum_int(~pop(vm).as.i));
            break;
        case PLUM_OP_SHL:
        case PLUM_OP_SHR: {
            int64_t b = pop(vm).as.i;
            int64_t a = pop(vm).as.i;
            if (b < 0 || b > 63) {
                FAIL("shift count %lld is out of range; it must be 0 to 63", (long long)b);
            }
            push(vm, plum_int(op == PLUM_OP_SHL ? (int64_t)((uint64_t)a << b) : shift_right(a, b)));
            break;
        }
        case PLUM_OP_LT_INT:
            INT_BINARY(plum_bool(a < b));
            break;
        case PLUM_OP_LE_INT:
            INT_BINARY(plum_bool(a <= b));
            break;
        case PLUM_OP_GT_INT:
            INT_BINARY(plum_bool(a > b));
            break;
        case PLUM_OP_GE_INT:
            INT_BINARY(plum_bool(a >= b));
            break;
        case PLUM_OP_EQ_INT:
            INT_BINARY(plum_bool(a == b));
            break;
        case PLUM_OP_NE_INT:
            INT_BINARY(plum_bool(a != b));
            break;

        case PLUM_OP_ADD_FLOAT:
            FLOAT_BINARY(plum_float(a + b));
            break;
        case PLUM_OP_SUB_FLOAT:
            FLOAT_BINARY(plum_float(a - b));
            break;
        case PLUM_OP_MUL_FLOAT:
            FLOAT_BINARY(plum_float(a * b));
            break;
        case PLUM_OP_DIV_FLOAT:
            FLOAT_BINARY(plum_float(a / b));
            break;
        case PLUM_OP_FLOORDIV_FLOAT:
            FLOAT_BINARY(plum_float(floor(a / b)));
            break;
        case PLUM_OP_MOD_FLOAT:
            FLOAT_BINARY(plum_float(float_mod(a, b)));
            break;
        case PLUM_OP_POW_FLOAT:
            FLOAT_BINARY(plum_float(pow(a, b)));
            break;
        case PLUM_OP_NEG_FLOAT:
            push(vm, plum_float(-pop(vm).as.f));
            break;
        case PLUM_OP_LT_FLOAT:
            FLOAT_BINARY(plum_bool(a < b));
            break;
        case PLUM_OP_LE_FLOAT:
            FLOAT_BINARY(plum_bool(a <= b));
            break;
        case PLUM_OP_GT_FLOAT:
            FLOAT_BINARY(plum_bool(a > b));
            break;
        case PLUM_OP_GE_FLOAT:
            FLOAT_BINARY(plum_bool(a >= b));
            break;
        case PLUM_OP_EQ_FLOAT:
            FLOAT_BINARY(plum_bool(a == b));
            break;
        case PLUM_OP_NE_FLOAT:
            FLOAT_BINARY(plum_bool(a != b));
            break;
        case PLUM_OP_INT_TO_FLOAT:
            push(vm, plum_float((double)pop(vm).as.i));
            break;

        case PLUM_OP_CONCAT: {
            PlumValue b = pop(vm);
            PlumValue a = pop(vm);
            PlumString *s = concat(plum_as_string(a), plum_as_string(b));
            plum_release(a);
            plum_release(b);
            push(vm, plum_obj(&s->obj));
            break;
        }
        case PLUM_OP_LT_STR:
            STR_COMPARE(c < 0);
            break;
        case PLUM_OP_LE_STR:
            STR_COMPARE(c <= 0);
            break;
        case PLUM_OP_GT_STR:
            STR_COMPARE(c > 0);
            break;
        case PLUM_OP_GE_STR:
            STR_COMPARE(c >= 0);
            break;
        case PLUM_OP_TO_STRING: {
            uint16_t f = READ_U16();
            PlumValue *v = peek(vm, 0);
            if (f == 0xFFFF && plum_is_obj(*v, PLUM_OBJ_STRING)) {
                break; /* already text */
            }
            const char *format =
                f == 0xFFFF ? NULL : plum_as_string(chunk->constants.items[f])->chars;
            char error[256];
            PlumString *s = plum_value_text(*v, format, error, sizeof error);
            if (s == NULL) {
                FAIL("%s", error);
            }
            plum_release(*v);
            *v = plum_obj(&s->obj);
            break;
        }
        case PLUM_OP_BUILD_STRING: {
            uint16_t n = READ_U16();
            size_t total = 0;
            for (uint16_t i = 0; i < n; i++) {
                total = plum_add_size(total, plum_as_string(*peek(vm, i))->len);
            }
            PlumString *s = plum_string_alloc(total);
            size_t at = total;
            for (uint16_t i = 0; i < n; i++) {
                PlumString *part = plum_as_string(*peek(vm, i));
                at -= part->len;
                memcpy(s->chars + at, part->chars, part->len);
            }
            unwind_to(vm, vm->top - n);
            push(vm, plum_obj(&s->obj));
            break;
        }

        case PLUM_OP_BUILD_LIST: {
            uint16_t n = READ_U16();
            PlumList *list = plum_list_new(n);
            if (n > 0) { /* memcpy with an empty list's NULL items is undefined */
                memcpy(list->items, &vm->stack[vm->top - n], n * sizeof(PlumValue)); /* refs move */
            }
            list->len = n;
            vm->top -= n;
            push(vm, plum_obj(&list->obj));
            break;
        }
        case PLUM_OP_INDEX_LIST: {
            int64_t i = pop(vm).as.i;
            PlumValue v = pop(vm);
            PlumList *list = plum_as_list(v);
            SAVE_IP(); /* so the error names this line */
            if (!check_index(vm, i, list->len)) {
                plum_release(v);
                return false;
            }
            push(vm, plum_retain(list->items[i]));
            plum_release(v);
            break;
        }
        case PLUM_OP_SLICE: {
            uint16_t inclusive = READ_U16();
            int64_t hi = pop(vm).as.i;
            int64_t lo = pop(vm).as.i;
            PlumValue v = pop(vm);
            PlumList *list = plum_as_list(v);
            int64_t end = inclusive ? hi + (hi < INT64_MAX) : hi;
            if (lo < 0 || lo > end || (uint64_t)end > list->len || (inclusive && hi == INT64_MAX)) {
                plum_release(v);
                FAIL("slice %lld..%s%lld is out of range for a list of %zu item%s", (long long)lo,
                     inclusive ? "=" : "", (long long)hi, list->len, list->len == 1 ? "" : "s");
            }
            PlumList *out = slice_list(list, (size_t)lo, (size_t)end);
            plum_release(v);
            push(vm, plum_obj(&out->obj));
            break;
        }
        case PLUM_OP_CONTAINS: {
            PlumValue coll = pop(vm);
            PlumValue item = pop(vm);
            PlumList *list = plum_as_list(coll);
            bool found = false;
            for (size_t i = 0; i < list->len && !found; i++) {
                found = plum_values_equal(list->items[i], item);
            }
            plum_release(coll);
            plum_release(item);
            push(vm, plum_bool(found));
            break;
        }
        case PLUM_OP_CONCAT_LIST: {
            PlumValue b = pop(vm);
            PlumValue a = pop(vm);
            PlumList *out = concat_lists(plum_as_list(a), plum_as_list(b));
            plum_release(a);
            plum_release(b);
            push(vm, plum_obj(&out->obj));
            break;
        }
        case PLUM_OP_METHOD:
        case PLUM_OP_METHOD_LOCAL:
        case PLUM_OP_METHOD_GLOBAL: {
            uint16_t slot = op == PLUM_OP_METHOD ? 0 : READ_U16();
            uint16_t m = READ_U16();
            uint16_t argc = READ_U16();
            PlumValue *receiver = op == PLUM_OP_METHOD         ? peek(vm, argc)
                                  : op == PLUM_OP_METHOD_LOCAL ? &SLOT(slot)
                                                               : &vm->globals[slot];
            PlumValue result;
            char error[256];
            bool ok = plum_run_method((PlumMethod)m, receiver, &vm->stack[vm->top - argc], argc,
                                      &result, error, sizeof error);
            unwind_to(vm, vm->top - argc - (op == PLUM_OP_METHOD ? 1 : 0));
            if (!ok) {
                FAIL("%s", error);
            }
            push(vm, result);
            break;
        }
        case PLUM_OP_SET_INDEX_LOCAL:
        case PLUM_OP_SET_INDEX_GLOBAL: {
            uint16_t slot = READ_U16();
            PlumValue value = pop(vm);
            int64_t i = pop(vm).as.i;
            PlumValue *place = op == PLUM_OP_SET_INDEX_LOCAL ? &SLOT(slot) : &vm->globals[slot];
            PlumList *list = plum_list_unique(place); /* copy first if shared */
            SAVE_IP();                                /* so the error names this line */
            if (!check_index(vm, i, list->len)) {
                plum_release(value);
                return false;
            }
            plum_release(list->items[i]);
            list->items[i] = value;
            break;
        }
        case PLUM_OP_FOR_EACH:
        case PLUM_OP_FOR_EACH2: {
            uint16_t at = READ_U16();
            uint16_t var = READ_U16();
            uint16_t d = READ_U16();
            PlumList *list = plum_as_list(SLOT(at));
            int64_t pos = SLOT(at + 1).as.i;
            if ((uint64_t)pos >= list->len) {
                ip += d;
                break;
            }
            SLOT(at + 1).as.i = pos + 1;
            if (op == PLUM_OP_FOR_EACH2) {
                SLOT(var) = plum_int(pos); /* ints hold no reference */
                var++;
            }
            plum_release(SLOT(var));
            SLOT(var) = plum_retain(list->items[pos]);
            break;
        }

        case PLUM_OP_EQ:
        case PLUM_OP_NE: {
            PlumValue b = pop(vm);
            PlumValue a = pop(vm);
            bool eq = plum_values_equal(a, b);
            plum_release(a);
            plum_release(b);
            push(vm, plum_bool(op == PLUM_OP_EQ ? eq : !eq));
            break;
        }
        case PLUM_OP_NOT:
            push(vm, plum_bool(!pop(vm).as.b));
            break;
        case PLUM_OP_IS_NONE: {
            PlumValue v = pop(vm);
            push(vm, plum_bool(v.type == PLUM_VAL_NONE));
            plum_release(v);
            break;
        }

        case PLUM_OP_JUMP: {
            uint16_t d = READ_U16();
            ip += d;
            break;
        }
        case PLUM_OP_JUMP_IF_FALSE: {
            uint16_t d = READ_U16();
            if (!pop(vm).as.b) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_FALSE_KEEP: {
            uint16_t d = READ_U16();
            if (!peek(vm, 0)->as.b) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_TRUE_KEEP: {
            uint16_t d = READ_U16();
            if (peek(vm, 0)->as.b) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_NOT_NONE_KEEP: {
            uint16_t d = READ_U16();
            if (peek(vm, 0)->type != PLUM_VAL_NONE) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_LOOP: {
            uint16_t d = READ_U16();
            ip -= d;
            break;
        }
        case PLUM_OP_FOR_RANGE: {
            uint16_t slot = READ_U16();
            uint16_t inclusive = READ_U16();
            uint16_t d = READ_U16();
            int64_t i = SLOT(slot).as.i, end = SLOT(slot + 1).as.i;
            if (inclusive ? i > end : i >= end) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_FOR_NEXT: {
            uint16_t slot = READ_U16();
            uint16_t inclusive = READ_U16();
            uint16_t d = READ_U16();
            /* At an inclusive end, stop before adding: it may be the
             * largest int. */
            if (!(inclusive && SLOT(slot).as.i == SLOT(slot + 1).as.i)) {
                SLOT(slot).as.i++;
                ip -= d;
            }
            break;
        }

        case PLUM_OP_CALL: {
            uint16_t argc = READ_U16();
            PlumFunction *fn = plum_as_function(*peek(vm, argc));
            SAVE_IP();
            if (!call(vm, fn, argc)) {
                return false;
            }
            frame = &vm->frames[vm->frame_count - 1];
            ip = frame->ip;
            chunk = frame->fn->chunk;
            break;
        }
        case PLUM_OP_CALL_BUILTIN: {
            uint16_t id = READ_U16();
            uint16_t argc = READ_U16();
            SAVE_IP();
            if (!call_builtin(vm, (PlumBuiltin)id, argc)) {
                return false;
            }
            if (vm->stopped) {
                return true; /* exit() */
            }
            break;
        }
        case PLUM_OP_RETURN:
        case PLUM_OP_RETURN_NONE: {
            PlumValue result = op == PLUM_OP_RETURN ? pop(vm) : plum_none();
            size_t base = frame->base;
            vm->frame_count--;
            if (vm->frame_count == 0) {
                plum_release(result);
                unwind_to(vm, 0);
                return true;
            }
            unwind_to(vm, base - 1); /* the slots and the function itself */
            push(vm, result);
            frame = &vm->frames[vm->frame_count - 1];
            ip = frame->ip;
            chunk = frame->fn->chunk;
            break;
        }
        case PLUM_OPCODE_COUNT:
        default:
            FAIL("internal error: unknown instruction %d", (int)op);
        }
    }

#undef READ_U16
#undef SLOT
#undef SAVE_IP
#undef FAIL
#undef INT_BINARY
#undef FLOAT_BINARY
#undef CHECKED
#undef STR_COMPARE
}

int plum_vm_run(const PlumCompiled *program, const PlumSource *source, FILE *out, FILE *err) {
    VM *vm = plum_alloc_zeroed(1, sizeof *vm);
    vm->source = source;
    vm->out = out;
    vm->err = err;
    vm->global_count = program->global_count;
    vm->globals = plum_alloc_zeroed(program->global_count > 0 ? (size_t)program->global_count : 1,
                                    sizeof(PlumValue));

    /* <main> sits at stack index 0, like any called function. */
    push(vm, plum_retain(plum_obj(&program->main->obj)));
    if (call(vm, program->main, 0)) {
        execute(vm);
    }
    fflush(out);

    unwind_to(vm, 0);
    for (int i = 0; i < vm->global_count; i++) {
        plum_release(vm->globals[i]);
    }
    int code = vm->exit_code;
    plum_free(vm->globals);
    plum_free(vm->stack);
    plum_free(vm);
    return code;
}
