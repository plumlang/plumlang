/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/vm.h"

#include "compiler/chunk.h"
#include "util/alloc.h"
#include "util/fmt.h"
#include "util/random.h"
#include "vm/methods.h"
#include "vm/table.h"
#include "vm/text.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct Frame {
    PlumFunction *fn;
    PlumClosure *closure; /* its captured variables; NULL for a plain function */
    const uint8_t *ip;
    size_t base; /* index of slot 0 in the stack; the function itself sits at base - 1 */
} Frame;

/* A try in progress: where to go, and what to unwind to, on an error. */
typedef struct Handler {
    int frame_count;       /* the frames the try's function sits on */
    size_t top;            /* the stack height when the try began */
    const uint8_t *target; /* where the catches start, with the error pushed */
} Handler;

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
    bool stopped;       /* exit() was called */
    int stop_at;        /* execute() returns when a RETURN leaves this many frames */
    int callback_depth; /* nested call_value()s */
    uint64_t seed[2];   /* the random key maps hash with */
    Handler *handlers;
    int handler_count;
    int handler_cap;
    PlumValue thrown;   /* the error being thrown, while unwinding */
    PlumValue trace_of; /* the error `trace` was written for */
    char *trace;        /* where it was thrown and the calls that led there */
    size_t trace_len;
    size_t trace_cap;
    PlumTreeInfo *const *errors; /* the built-in error trees */
    int to_str;                  /* the method number of to_str */
    const PlumRunOptions *options;
    PLUM_ARRAY(PlumWeak *) tracked; /* --check-leaks: a weak box for every tree object */
    PlumTextHook text_hook;         /* objects become text through their to_str() */
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

PLUM_PRINTF(2, 3)
static void trace_add(VM *vm, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char line[1024];
    int n = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    size_t len = n < 0 ? 0 : (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    if (vm->trace_len + len + 1 > vm->trace_cap) {
        size_t cap = vm->trace_cap < 256 ? 256 : vm->trace_cap;
        while (cap < vm->trace_len + len + 1) {
            cap = plum_mul_size(cap, 2);
        }
        vm->trace = plum_realloc_array(vm->trace, cap, 1);
        vm->trace_cap = cap;
    }
    memcpy(vm->trace + vm->trace_len, line, len);
    vm->trace_len += len;
    vm->trace[vm->trace_len] = '\0';
}

/*
 * Throw `error` (an object of an error tree; the reference moves here).
 * Where it was first thrown is written down now, while the calls that led
 * there are still on the stack: if nothing catches it, that is what the
 * program prints. Rethrowing the same error keeps the first place.
 */
static void throw_error(VM *vm, PlumValue error) {
    plum_release(vm->thrown);
    vm->thrown = error;
    if (vm->trace_of.type == PLUM_VAL_OBJ && vm->trace_of.as.obj == error.as.obj) {
        return;
    }
    plum_release(vm->trace_of);
    vm->trace_of = plum_retain(error);
    vm->trace_len = 0;
    const PlumObject *o = plum_as_object(error);
    const PlumString *message = plum_as_string(o->fields[0]); /* every error's first field */
    const Frame *top = &vm->frames[vm->frame_count - 1];
    trace_add(vm, "%s:%u: %s: %s\n", vm->source->path, (unsigned)frame_line(top), o->tree->name,
              message->chars);
    /* Innermost calls first; a deep recursion shows its two ends. */
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        int from_top = vm->frame_count - 1 - i;
        if (from_top == 10 && i > 2) {
            /* frames i down to 2 are skipped; 1 and 0 are shown */
            trace_add(vm, "  ... %d more calls\n", i - 1);
            i = 1;
        }
        const Frame *f = &vm->frames[i];
        trace_add(vm, "  in %s, line %u\n", f->fn->name, (unsigned)frame_line(f));
    }
}

/* Raise a built-in error of `kind` with a formatted message. */
PLUM_PRINTF(3, 4)
static void runtime_error(VM *vm, PlumErrorKind kind, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char message[1024];
    int n = vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    size_t len = n < 0 ? 0 : (size_t)n < sizeof message ? (size_t)n : sizeof message - 1;
    PlumObject *o = plum_object_new(vm->errors[kind]);
    o->fields[0] = plum_obj(&plum_string_new(message, len)->obj);
    throw_error(vm, plum_obj(&o->obj));
}

/*
 * After an error was thrown: unwind to the innermost try of this run of
 * the loop and continue at its catches with the error pushed. False when
 * no try here catches it (or exit() was called): execute() returns, and
 * whoever started it carries on unwinding.
 */
static void unwind_to(VM *vm, size_t to);

static bool catch_thrown(VM *vm) {
    if (vm->stopped || vm->handler_count == 0) {
        return false;
    }
    const Handler *h = &vm->handlers[vm->handler_count - 1];
    if (h->frame_count <= vm->stop_at) {
        return false; /* it belongs to code that called into this run */
    }
    vm->handler_count--;
    vm->frame_count = h->frame_count;
    unwind_to(vm, h->top);
    push(vm, vm->thrown);
    vm->thrown = plum_none();
    vm->frames[vm->frame_count - 1].ip = h->target;
    return true;
}

/* Print an error nothing caught; the program ends with exit code 1. */
static void report_uncaught(VM *vm) {
    fflush(vm->out); /* the program's output so far comes before the error */
    if (vm->trace != NULL) {
        fputs(vm->trace, vm->err);
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
        case PLUM_OBJ_CLOSURE:
        case PLUM_OBJ_CELL:
            return "branch";
        case PLUM_OBJ_OBJECT: {
            const PlumTreeInfo *tree = plum_as_object(v)->tree;
            return tree->kind != NULL ? tree->kind : tree->name;
        }
        case PLUM_OBJ_TREE_INFO:
            return "tree";
        case PLUM_OBJ_WEAK:
            return "weak";
        case PLUM_OBJ_LIST:
            return "list";
        case PLUM_OBJ_MAP:
            return plum_as_table(v)->is_set ? "set" : "map";
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
    case PLUM_BUILTIN_DROP: {
        /* All the text first: a to_str() may print, or throw, or grow the stack. */
        size_t first = vm->top - (size_t)argc;
        PLUM_ARRAY(char) line = {0};
        for (int i = 0; i < argc; i++) {
            PlumString *s = plum_value_text_with(vm->stack[first + (size_t)i], NULL, &vm->text_hook,
                                                 error, sizeof error);
            if (s == NULL) {
                plum_array_free(&line);
                return false; /* to_str() threw */
            }
            if (i > 0) {
                plum_array_push(&line, ' ');
            }
            for (size_t k = 0; k < s->len; k++) {
                plum_array_push(&line, s->chars[k]);
            }
            plum_release(plum_obj(&s->obj));
        }
        plum_array_push(&line, '\n');
        fwrite(line.items, 1, line.len, vm->out);
        plum_array_free(&line);
        break;
    }
    case PLUM_BUILTIN_LEN:
        if (plum_is_obj(args[0], PLUM_OBJ_LIST)) {
            result = plum_int((int64_t)plum_as_list(args[0])->len);
        } else if (plum_is_obj(args[0], PLUM_OBJ_MAP)) {
            result = plum_int((int64_t)plum_as_table(args[0])->count);
        } else {
            result = plum_int((int64_t)plum_string_length(plum_as_string(args[0])));
        }
        break;
    case PLUM_BUILTIN_STR: {
        PlumString *s = plum_value_text_with(args[0], NULL, &vm->text_hook, error, sizeof error);
        if (s == NULL) {
            return false; /* to_str() threw */
        }
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
                runtime_error(vm, PLUM_ERROR_OVERFLOW, "int(%s) doesn't fit in an int",
                              plum_format_float(f, buf));
                return false;
            }
            result = plum_int((int64_t)f); /* truncates toward zero */
        } else {
            int64_t i;
            if (!parse_int(plum_as_string(v), &i)) {
                runtime_error(vm, PLUM_ERROR_PARSE, "int(\"%s\"): not a whole number",
                              plum_as_string(v)->chars);
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
                runtime_error(vm, PLUM_ERROR_PARSE, "float(\"%s\"): not a number",
                              plum_as_string(v)->chars);
                return false;
            }
            result = plum_float(f);
        }
        break;
    }
    case PLUM_BUILTIN_BYTE:
        if (args[0].as.i < 0 || args[0].as.i > 255) {
            runtime_error(vm, PLUM_ERROR_VALUE, "byte(%lld) is out of range; a byte is 0 to 255",
                          (long long)args[0].as.i);
            return false;
        }
        result = args[0];
        break;
    case PLUM_BUILTIN_SET: {
        PlumTable *s = plum_table_new(true, vm->seed);
        PlumList *items = plum_as_list(args[0]);
        for (size_t i = 0; i < items->len; i++) {
            plum_table_put(s, plum_retain(items->items[i]), plum_none());
        }
        result = plum_obj(&s->obj);
        break;
    }
    case PLUM_BUILTIN_TYPE_OF: {
        const char *name = type_name(args[0]);
        result = str_value(plum_string_new(name, strlen(name)));
        break;
    }
    case PLUM_BUILTIN_ASSERT:
        if (!args[0].as.b) {
            if (argc > 1 && plum_as_string(args[1])->len > 0) {
                runtime_error(vm, PLUM_ERROR_ASSERT, "assertion failed: %s",
                              plum_as_string(args[1])->chars);
            } else {
                runtime_error(vm, PLUM_ERROR_ASSERT, "assertion failed");
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
            PlumString *s =
                plum_value_text_with(args[0], NULL, &vm->text_hook, error, sizeof error);
            if (s == NULL) {
                return false; /* to_str() threw */
            }
            fwrite(s->chars, 1, s->len, vm->out);
            plum_release(plum_obj(&s->obj));
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
            runtime_error(vm, PLUM_ERROR_INDEX, "index %lld is out of range: the list is empty",
                          (long long)i);
        } else {
            runtime_error(vm, PLUM_ERROR_INDEX,
                          "index %lld is out of range: the list has %zu item%s (0 to %zu)",
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

/*
 * Walk `depth` keys down from `*place` (grid[i][j], m["k"][0]), making each
 * level unique on the way so a change copies only what is shared. Returns
 * the item's slot, or NULL after reporting a bad index or missing key.
 */
static PlumValue *walk_path(VM *vm, PlumValue *place, const PlumValue *keys, int depth) {
    for (int d = 0; d < depth; d++) {
        if (plum_is_obj(*place, PLUM_OBJ_MAP)) {
            PlumValue *next = plum_table_find(plum_table_unique(place), keys[d]);
            if (next == NULL) {
                char error[16];
                PlumString *k = plum_value_text(keys[d], NULL, error, sizeof error);
                const char *q = plum_is_obj(keys[d], PLUM_OBJ_STRING) ? "\"" : "";
                runtime_error(vm, PLUM_ERROR_KEY, "the key %s%s%s is not in the map", q, k->chars,
                              q);
                plum_release(plum_obj(&k->obj));
                return NULL;
            }
            place = next;
        } else {
            PlumList *list = plum_list_unique(place);
            if (!check_index(vm, keys[d].as.i, list->len)) {
                return NULL;
            }
            place = &list->items[keys[d].as.i];
        }
    }
    return place;
}

static bool execute(VM *vm);
static bool call(VM *vm, PlumFunction *fn, PlumClosure *closure, int argc);

/*
 * Call a Plumlang function from native code (map, filter, ...): run the
 * loop until that call returns. Arguments are borrowed. On a runtime error
 * or exit() returns false, the error already reported.
 */
static bool call_value(VM *vm, PlumValue fn, const PlumValue *args, int argc, PlumValue *out) {
    if (vm->callback_depth == PLUM_MAX_CALLBACK_DEPTH) {
        runtime_error(vm, PLUM_ERROR_ERROR,
                      "too much recursion: more than %d calls nested inside to_str, map and "
                      "other callbacks",
                      PLUM_MAX_CALLBACK_DEPTH);
        return false;
    }
    push(vm, plum_retain(fn));
    grow(vm, vm->top + (size_t)argc);
    for (int i = 0; i < argc; i++) {
        push(vm, plum_retain(args[i]));
    }
    PlumClosure *closure = fn.as.obj->type == PLUM_OBJ_CLOSURE ? plum_as_closure(fn) : NULL;
    PlumFunction *f = closure != NULL ? closure->fn : plum_as_function(fn);
    int saved = vm->stop_at;
    vm->stop_at = vm->frame_count;
    vm->callback_depth++;
    bool ok = call(vm, f, closure, argc) && execute(vm) && !vm->stopped;
    vm->callback_depth--;
    vm->stop_at = saved;
    if (!ok) {
        return false;
    }
    *out = pop(vm);
    return true;
}

/*
 * map, filter, reduce, any, all, find and sorted_by. They hold their own
 * references to the list and the function: a callback may grow the stack,
 * so nothing here points into it across a call.
 */
static bool run_higher_order(VM *vm, PlumMethod m, PlumList *list, const PlumValue *args,
                             PlumValue *result) {
    PlumValue fn = args[0];
    PlumValue r;
    switch (m) {
    case PLUM_METHOD_LIST_MAP:
    case PLUM_METHOD_LIST_FILTER: {
        PlumList *out = plum_list_new(m == PLUM_METHOD_LIST_MAP ? list->len : 0);
        for (size_t i = 0; i < list->len; i++) {
            if (!call_value(vm, fn, &list->items[i], 1, &r)) {
                plum_release(plum_obj(&out->obj));
                return false;
            }
            if (m == PLUM_METHOD_LIST_MAP) {
                plum_list_push(out, r);
            } else if (r.as.b) {
                plum_list_push(out, plum_retain(list->items[i]));
            }
        }
        *result = plum_obj(&out->obj);
        return true;
    }
    case PLUM_METHOD_LIST_REDUCE: {
        PlumValue total = plum_retain(args[1]);
        for (size_t i = 0; i < list->len; i++) {
            PlumValue pair[2] = {total, list->items[i]};
            bool ok = call_value(vm, fn, pair, 2, &r);
            plum_release(total);
            if (!ok) {
                return false;
            }
            total = r;
        }
        *result = total;
        return true;
    }
    case PLUM_METHOD_LIST_ANY:
    case PLUM_METHOD_LIST_ALL:
    case PLUM_METHOD_LIST_FIND: {
        bool want = m != PLUM_METHOD_LIST_ALL; /* stop at the first item that is (not) a match */
        for (size_t i = 0; i < list->len; i++) {
            if (!call_value(vm, fn, &list->items[i], 1, &r)) {
                return false;
            }
            if (r.as.b == want) {
                *result = m == PLUM_METHOD_LIST_FIND ? plum_retain(list->items[i])
                                                     : plum_bool(m == PLUM_METHOD_LIST_ANY);
                return true;
            }
        }
        *result = m == PLUM_METHOD_LIST_FIND ? plum_none() : plum_bool(m == PLUM_METHOD_LIST_ALL);
        return true;
    }
    case PLUM_METHOD_LIST_SORTED_BY: {
        /* the key of every item first, then a stable sort of (key, item) */
        size_t n = list->len;
        PlumList *keys = plum_list_new(n);
        for (size_t i = 0; i < n; i++) {
            if (!call_value(vm, fn, &list->items[i], 1, &r)) {
                plum_release(plum_obj(&keys->obj));
                return false;
            }
            plum_list_push(keys, r);
        }
        size_t *order = plum_alloc_zeroed(n > 0 ? n : 1, sizeof(size_t));
        size_t *tmp = plum_alloc_zeroed(n > 0 ? n : 1, sizeof(size_t));
        for (size_t i = 0; i < n; i++) {
            order[i] = i;
        }
        /* bottom-up merge sort of positions by key: stable */
        for (size_t width = 1; width < n; width *= 2) {
            for (size_t lo = 0; lo < n; lo += 2 * width) {
                size_t mid = lo + width < n ? lo + width : n;
                size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
                size_t i = lo, j = mid, k = lo;
                while (i < mid && j < hi) {
                    tmp[k++] =
                        plum_compare_values(keys->items[order[i]], keys->items[order[j]]) <= 0
                            ? order[i++]
                            : order[j++];
                }
                while (i < mid) {
                    tmp[k++] = order[i++];
                }
                while (j < hi) {
                    tmp[k++] = order[j++];
                }
            }
            memcpy(order, tmp, n * sizeof(size_t));
        }
        PlumList *out = plum_list_new(n);
        for (size_t i = 0; i < n; i++) {
            plum_list_push(out, plum_retain(list->items[order[i]]));
        }
        plum_free(order);
        plum_free(tmp);
        plum_release(plum_obj(&keys->obj));
        *result = plum_obj(&out->obj);
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Methods                                                             */
/* ------------------------------------------------------------------ */

static PlumFunction *method_of(const PlumTreeInfo *tree, int id) {
    return id >= 0 && id < tree->method_count ? tree->methods[id] : NULL;
}

/* An object's text: its to_str(), if its tree has one. */
static bool object_text(void *ctx, PlumValue object, PlumString **out) {
    VM *vm = ctx;
    *out = NULL;
    PlumFunction *fn = method_of(plum_as_object(object)->tree, vm->to_str);
    if (fn == NULL) {
        return true;
    }
    PlumValue text;
    if (!call_value(vm, plum_obj(&fn->obj), &object, 1, &text)) {
        return false;
    }
    *out = plum_as_string(text);
    return true;
}

/*
 * Call `fn` as a method: the object sits under its `argc` arguments, and
 * the function goes under the object, where a call expects it.
 */
static bool invoke(VM *vm, PlumFunction *fn, int argc) {
    grow(vm, vm->top + 1);
    size_t at = vm->top - (size_t)argc - 1; /* the object */
    memmove(&vm->stack[at + 1], &vm->stack[at], ((size_t)argc + 1) * sizeof(PlumValue));
    vm->stack[at] = plum_retain(plum_obj(&fn->obj));
    vm->top++;
    return call(vm, fn, NULL, argc + 1);
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

static bool call(VM *vm, PlumFunction *fn, PlumClosure *closure, int argc) {
    if (vm->frame_count == PLUM_MAX_FRAMES) {
        runtime_error(vm, PLUM_ERROR_ERROR, "too much recursion: more than %d calls deep",
                      PLUM_MAX_FRAMES);
        return false;
    }
    Frame *f = &vm->frames[vm->frame_count++];
    f->fn = fn;
    f->closure = closure;
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

/*
 * The value an in-place instruction changes: a local slot (looking through
 * a cell if a closure captured the variable), a global, or a variable the
 * running closure captured.
 */
static PlumValue *place_of(VM *vm, Frame *frame, int kind, uint16_t n) {
    PlumValue *p;
    if (kind == 0) {
        p = &vm->stack[frame->base + n];
        if (plum_is_obj(*p, PLUM_OBJ_CELL)) {
            p = &plum_as_cell(*p)->value;
        }
    } else if (kind == 1) {
        p = &vm->globals[n];
    } else {
        p = &frame->closure->cells[n]->value;
    }
    return p;
}

#define PLACE_KIND(op, local, global) ((op) == (local) ? 0 : (op) == (global) ? 1 : 2)

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
        goto thrown;                    \
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
#define CHECKED(fn, symbol)                                                                   \
    do {                                                                                      \
        int64_t b = pop(vm).as.i;                                                             \
        int64_t a = pop(vm).as.i;                                                             \
        int64_t r;                                                                            \
        if (fn(a, b, &r)) {                                                                   \
            FAIL(PLUM_ERROR_OVERFLOW, "integer overflow: %lld %s %lld doesn't fit in an int", \
                 (long long)a, symbol, (long long)b);                                         \
        }                                                                                     \
        push(vm, plum_int(r));                                                                \
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
        case PLUM_OP_BOX_LOCAL: {
            uint16_t n = READ_U16();
            SLOT(n) = plum_obj(&plum_cell_new(SLOT(n))->obj); /* the cell takes the value */
            break;
        }
        case PLUM_OP_GET_CELL:
            push(vm, plum_retain(plum_as_cell(SLOT(READ_U16()))->value));
            break;
        case PLUM_OP_SET_CELL: {
            PlumCell *cell = plum_as_cell(SLOT(READ_U16()));
            PlumValue v = pop(vm);
            plum_release(cell->value);
            cell->value = v;
            break;
        }
        case PLUM_OP_GET_CAPTURE:
            push(vm, plum_retain(frame->closure->cells[READ_U16()]->value));
            break;
        case PLUM_OP_SET_CAPTURE: {
            PlumCell *cell = frame->closure->cells[READ_U16()];
            PlumValue v = pop(vm);
            plum_release(cell->value);
            cell->value = v;
            break;
        }
        case PLUM_OP_GET_SELF:
            push(vm, plum_retain(vm->stack[frame->base - 1])); /* the callee sits below slot 0 */
            break;
        case PLUM_OP_MAKE_CLOSURE: {
            PlumFunction *fn = plum_as_function(chunk->constants.items[READ_U16()]);
            PlumClosure *closure = plum_closure_new(fn);
            for (int i = 0; i < fn->capture_count; i++) {
                const PlumCapture *cap = &fn->captures[i];
                PlumCell *cell = cap->from_local ? plum_as_cell(SLOT(cap->index))
                                                 : frame->closure->cells[cap->index];
                cell->obj.refcount++;
                closure->cells[i] = cell;
            }
            push(vm, plum_obj(&closure->obj));
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
                FAIL(PLUM_ERROR_ZERO_DIVISION, "division by zero: %lld %s 0", (long long)a, sym);
            }
            if (op == PLUM_OP_FLOORDIV_INT && a == INT64_MIN && b == -1) {
                FAIL(PLUM_ERROR_OVERFLOW, "integer overflow: %lld // -1 doesn't fit in an int",
                     (long long)a);
            }
            push(vm, plum_int(op == PLUM_OP_FLOORDIV_INT ? floor_div(a, b) : floor_mod(a, b)));
            break;
        }
        case PLUM_OP_POW_INT: {
            int64_t b = pop(vm).as.i;
            int64_t a = pop(vm).as.i;
            int64_t r;
            if (b < 0) {
                FAIL(PLUM_ERROR_VALUE,
                     "%lld ** %lld: an int power needs an exponent of 0 or more; use a float "
                     "for fractions",
                     (long long)a, (long long)b);
            }
            if (pow_overflows(a, b, &r)) {
                FAIL(PLUM_ERROR_OVERFLOW, "integer overflow: %lld ** %lld doesn't fit in an int",
                     (long long)a, (long long)b);
            }
            push(vm, plum_int(r));
            break;
        }
        case PLUM_OP_NEG_INT: {
            int64_t a = pop(vm).as.i;
            if (a == INT64_MIN) {
                FAIL(PLUM_ERROR_OVERFLOW, "integer overflow: -(%lld) doesn't fit in an int",
                     (long long)a);
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
                FAIL(PLUM_ERROR_VALUE, "shift count %lld is out of range; it must be 0 to 63",
                     (long long)b);
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
            SAVE_IP(); /* to_str() may run */
            PlumString *s = plum_value_text_with(*v, format, &vm->text_hook, error, sizeof error);
            if (s == NULL && error[0] == '\0') {
                goto thrown; /* to_str() threw */
            }
            if (s == NULL) {
                FAIL(PLUM_ERROR_VALUE, "%s", error);
            }
            v = peek(vm, 0); /* the stack may have moved */
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

        case PLUM_OP_BUILD_MAP: {
            uint16_t n = READ_U16(); /* pairs: key, value, key, value ... */
            PlumTable *table = plum_table_new(false, vm->seed);
            PlumValue *pairs = &vm->stack[vm->top - 2 * (size_t)n];
            for (uint16_t i = 0; i < n; i++) {
                plum_table_put(table, pairs[2 * i], pairs[2 * i + 1]); /* refs move */
            }
            vm->top -= 2 * (size_t)n;
            push(vm, plum_obj(&table->obj));
            break;
        }
        case PLUM_OP_INDEX_STR: {
            int64_t i = pop(vm).as.i;
            PlumValue v = pop(vm);
            PlumString *s = plum_as_string(v);
            size_t n = plum_string_length(s);
            if (i < 0 || (uint64_t)i >= n) {
                plum_release(v);
                if (n == 0) {
                    FAIL(PLUM_ERROR_INDEX, "index %lld is out of range: the string is empty",
                         (long long)i);
                }
                FAIL(PLUM_ERROR_INDEX,
                     "index %lld is out of range: the string has %zu character%s (0 to %zu)",
                     (long long)i, n, n == 1 ? "" : "s", n - 1);
            }
            size_t from = plum_string_offset(s, (size_t)i);
            size_t to = plum_string_offset(s, (size_t)i + 1);
            PlumString *ch = plum_string_new(s->chars + from, to - from);
            plum_release(v);
            push(vm, plum_obj(&ch->obj));
            break;
        }
        case PLUM_OP_INDEX_MAP: {
            PlumValue key = pop(vm);
            PlumValue m = pop(vm);
            PlumValue *found = plum_table_find(plum_as_table(m), key);
            if (found == NULL) {
                char error[16];
                PlumString *k = plum_value_text(key, NULL, error, sizeof error);
                plum_release(m);
                plum_release(key);
                SAVE_IP();
                runtime_error(vm, PLUM_ERROR_KEY,
                              "the key %s%s%s is not in the map; use .get(key) for a value "
                              "that may be missing",
                              plum_is_obj(key, PLUM_OBJ_STRING) ? "\"" : "", k->chars,
                              plum_is_obj(key, PLUM_OBJ_STRING) ? "\"" : "");
                plum_release(plum_obj(&k->obj));
                goto thrown;
            }
            push(vm, plum_retain(*found));
            plum_release(m);
            plum_release(key);
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
                goto thrown;
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
            bool text = plum_is_obj(v, PLUM_OBJ_STRING);
            size_t size = text ? plum_string_length(plum_as_string(v)) : plum_as_list(v)->len;
            int64_t end = inclusive ? hi + (hi < INT64_MAX) : hi;
            if (lo < 0 || lo > end || (uint64_t)end > size || (inclusive && hi == INT64_MAX)) {
                plum_release(v);
                FAIL(PLUM_ERROR_INDEX, "slice %lld..%s%lld is out of range for a %s of %zu %s%s",
                     (long long)lo, inclusive ? "=" : "", (long long)hi, text ? "string" : "list",
                     size, text ? "character" : "item", size == 1 ? "" : "s");
            }
            PlumValue out;
            if (text) {
                PlumString *s = plum_as_string(v);
                size_t from = plum_string_offset(s, (size_t)lo);
                size_t to = plum_string_offset(s, (size_t)end);
                out = plum_obj(&plum_string_new(s->chars + from, to - from)->obj);
            } else {
                out = plum_obj(&slice_list(plum_as_list(v), (size_t)lo, (size_t)end)->obj);
            }
            plum_release(v);
            push(vm, out);
            break;
        }
        case PLUM_OP_CONTAINS: {
            PlumValue coll = pop(vm);
            PlumValue item = pop(vm);
            bool found = false;
            if (plum_is_obj(coll, PLUM_OBJ_MAP)) {
                found = plum_table_find(plum_as_table(coll), item) != NULL; /* a key */
            } else if (plum_is_obj(coll, PLUM_OBJ_STRING)) {
                const PlumString *hay = plum_as_string(coll), *needle = plum_as_string(item);
                found = needle->len == 0;
                for (size_t i = 0; !found && i + needle->len <= hay->len; i++) {
                    found = memcmp(hay->chars + i, needle->chars, needle->len) == 0;
                }
            } else {
                PlumList *list = plum_as_list(coll);
                for (size_t i = 0; i < list->len && !found; i++) {
                    found = plum_values_equal(list->items[i], item);
                }
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
        case PLUM_OP_METHOD_GLOBAL:
        case PLUM_OP_METHOD_CAPTURE: {
            uint16_t slot = op == PLUM_OP_METHOD ? 0 : READ_U16();
            uint16_t m = READ_U16();
            uint16_t argc = READ_U16();
            PlumValue *receiver =
                op == PLUM_OP_METHOD
                    ? peek(vm, argc)
                    : place_of(vm, frame,
                               PLACE_KIND(op, PLUM_OP_METHOD_LOCAL, PLUM_OP_METHOD_GLOBAL), slot);
            PlumValue result;
            char error[256];
            if (plum_method_calls_back((PlumMethod)m)) {
                /* own references to everything, then off the stack: the
                 * callbacks may move it */
                PlumValue list = plum_retain(*receiver);
                PlumValue args[2] = {plum_none(), plum_none()};
                for (uint16_t i = 0; i < argc && i < 2; i++) {
                    args[i] = plum_retain(vm->stack[vm->top - argc + i]);
                }
                unwind_to(vm, vm->top - argc - (op == PLUM_OP_METHOD ? 1 : 0));
                SAVE_IP();
                bool done = run_higher_order(vm, (PlumMethod)m, plum_as_list(list), args, &result);
                plum_release(list);
                plum_release(args[0]);
                plum_release(args[1]);
                if (!done) {
                    goto thrown;
                }
                push(vm, result);
                break;
            }
            bool ok = plum_run_method((PlumMethod)m, receiver, &vm->stack[vm->top - argc], argc,
                                      &result, error, sizeof error);
            unwind_to(vm, vm->top - argc - (op == PLUM_OP_METHOD ? 1 : 0));
            if (!ok) {
                FAIL(PLUM_ERROR_INDEX, "%s", error);
            }
            push(vm, result);
            break;
        }
        case PLUM_OP_SET_INDEX_LOCAL:
        case PLUM_OP_SET_INDEX_GLOBAL:
        case PLUM_OP_SET_INDEX_CAPTURE: {
            uint16_t slot = READ_U16();
            PlumValue value = pop(vm);
            PlumValue *place = place_of(
                vm, frame, PLACE_KIND(op, PLUM_OP_SET_INDEX_LOCAL, PLUM_OP_SET_INDEX_GLOBAL), slot);
            if (plum_is_obj(*place, PLUM_OBJ_MAP)) {
                PlumValue key = pop(vm);
                plum_table_put(plum_table_unique(place), key, value); /* copy first if shared */
                break;
            }
            int64_t i = pop(vm).as.i;
            PlumList *list = plum_list_unique(place); /* copy first if shared */
            SAVE_IP();                                /* so the error names this line */
            if (!check_index(vm, i, list->len)) {
                plum_release(value);
                goto thrown;
            }
            plum_release(list->items[i]);
            list->items[i] = value;
            break;
        }
        case PLUM_OP_SET_PATH_LOCAL:
        case PLUM_OP_SET_PATH_GLOBAL:
        case PLUM_OP_SET_PATH_CAPTURE:
        case PLUM_OP_SET_PATH_FIELD: {
            uint16_t slot = READ_U16();
            uint16_t depth = READ_U16();
            PlumValue value = pop(vm);
            PlumValue *keys = &vm->stack[vm->top - depth];
            /* a field's object sits under the keys */
            PlumValue *root =
                op == PLUM_OP_SET_PATH_FIELD
                    ? &plum_as_object(vm->stack[vm->top - depth - 1])->fields[slot]
                    : place_of(vm, frame,
                               PLACE_KIND(op, PLUM_OP_SET_PATH_LOCAL, PLUM_OP_SET_PATH_GLOBAL),
                               slot);
            SAVE_IP();
            /* down to the container of the item, then set it */
            PlumValue *container = walk_path(vm, root, keys, depth - 1);
            if (container == NULL) {
                plum_release(value);
                goto thrown;
            }
            PlumValue key = keys[depth - 1];
            if (plum_is_obj(*container, PLUM_OBJ_MAP)) {
                plum_table_put(plum_table_unique(container), plum_retain(key), value);
            } else {
                PlumList *list = plum_list_unique(container);
                if (!check_index(vm, key.as.i, list->len)) {
                    plum_release(value);
                    goto thrown;
                }
                plum_release(list->items[key.as.i]);
                list->items[key.as.i] = value;
            }
            unwind_to(vm, vm->top - depth - (op == PLUM_OP_SET_PATH_FIELD ? 1 : 0));
            break;
        }
        case PLUM_OP_METHOD_PATH_LOCAL:
        case PLUM_OP_METHOD_PATH_GLOBAL:
        case PLUM_OP_METHOD_PATH_CAPTURE:
        case PLUM_OP_METHOD_PATH_FIELD: {
            uint16_t slot = READ_U16();
            uint16_t m = READ_U16();
            uint16_t packed = READ_U16();
            int depth = packed >> 8, argc = packed & 0xFF;
            size_t under = op == PLUM_OP_METHOD_PATH_FIELD ? 1 : 0; /* the field's object */
            PlumValue *keys = &vm->stack[vm->top - (size_t)argc - (size_t)depth];
            PlumValue *root = under ? &plum_as_object(keys[-1])->fields[slot]
                                    : place_of(vm, frame,
                                               PLACE_KIND(op, PLUM_OP_METHOD_PATH_LOCAL,
                                                          PLUM_OP_METHOD_PATH_GLOBAL),
                                               slot);
            SAVE_IP();
            PlumValue *receiver = walk_path(vm, root, keys, depth);
            if (receiver == NULL) {
                goto thrown;
            }
            PlumValue result;
            char error[256];
            bool ok = plum_run_method((PlumMethod)m, receiver, &vm->stack[vm->top - (size_t)argc],
                                      argc, &result, error, sizeof error);
            unwind_to(vm, vm->top - (size_t)argc - (size_t)depth - under);
            if (!ok) {
                FAIL(PLUM_ERROR_INDEX, "%s", error);
            }
            push(vm, result);
            break;
        }
        case PLUM_OP_FOR_EACH:
        case PLUM_OP_FOR_EACH2: {
            uint16_t at = READ_U16();
            uint16_t var = READ_U16();
            uint16_t d = READ_U16();
            if (plum_is_obj(SLOT(at), PLUM_OBJ_STRING)) {
                const PlumString *s = plum_as_string(SLOT(at));
                size_t from = (size_t)SLOT(at + 1).as.i; /* a byte offset */
                if (from >= s->len) {
                    ip += d;
                    break;
                }
                size_t to = from + 1;
                while (to < s->len && ((unsigned char)s->chars[to] & 0xC0) == 0x80) {
                    to++;
                }
                SLOT(at + 1).as.i = (int64_t)to;
                plum_release(SLOT(var));
                SLOT(var) = plum_obj(&plum_string_new(s->chars + from, to - from)->obj);
                break;
            }
            if (plum_is_obj(SLOT(at), PLUM_OBJ_MAP)) {
                /* maps give keys (and values with two names); sets give items */
                int64_t pos = SLOT(at + 1).as.i;
                const PlumTableEntry *e = plum_table_next(plum_as_table(SLOT(at)), &pos);
                if (e == NULL) {
                    ip += d;
                    break;
                }
                SLOT(at + 1).as.i = pos;
                plum_release(SLOT(var));
                SLOT(var) = plum_retain(e->key);
                if (op == PLUM_OP_FOR_EACH2) {
                    plum_release(SLOT(var + 1));
                    SLOT(var + 1) = plum_retain(e->value);
                }
                break;
            }
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
        case PLUM_OP_NEW_OBJECT: {
            PlumTreeInfo *tree = plum_as_tree_info(chunk->constants.items[READ_U16()]);
            uint16_t n = READ_U16();
            PlumObject *o = plum_object_new(tree);
            for (uint16_t i = 0; i < n && i < tree->field_count; i++) {
                o->fields[i] = vm->stack[vm->top - n + i]; /* refs move */
            }
            vm->top -= n;
            if (vm->options->check_leaks && tree->kind == NULL) {
                SAVE_IP();
                PlumWeak *w = plum_weak_of(o);
                w->line = frame_line(frame);
                plum_array_push(&vm->tracked, w);
            }
            push(vm, plum_obj(&o->obj));
            break;
        }
        case PLUM_OP_GET_FIELD: {
            uint16_t i = READ_U16();
            PlumValue v = pop(vm);
            push(vm, plum_retain(plum_as_object(v)->fields[i]));
            plum_release(v);
            break;
        }
        case PLUM_OP_SET_FIELD: {
            uint16_t i = READ_U16();
            PlumValue value = pop(vm);
            PlumValue v = pop(vm);
            PlumObject *o = plum_as_object(v);
            PlumValue old = o->fields[i];
            o->fields[i] = value;
            plum_release(old);
            plum_release(v);
            break;
        }
        case PLUM_OP_IS_TREE: {
            PlumTreeInfo *tree = plum_as_tree_info(chunk->constants.items[READ_U16()]);
            PlumValue v = pop(vm);
            bool is = v.type == PLUM_VAL_OBJ && v.as.obj->type == PLUM_OBJ_OBJECT &&
                      plum_tree_is(plum_as_object(v)->tree, tree);
            push(vm, plum_bool(is));
            plum_release(v);
            break;
        }
        case PLUM_OP_WEAK: {
            PlumValue *v = peek(vm, 0);
            if (v->type == PLUM_VAL_OBJ) {
                PlumWeak *w = plum_weak_of(plum_as_object(*v));
                plum_release(*v);
                *v = plum_obj(&w->obj);
            }
            break;
        }
        case PLUM_OP_UNWEAK: {
            PlumValue *v = peek(vm, 0);
            if (v->type == PLUM_VAL_OBJ) {
                PlumObject *target = ((PlumWeak *)v->as.obj)->target;
                PlumValue strong =
                    target != NULL ? plum_retain(plum_obj(&target->obj)) : plum_none();
                plum_release(*v);
                *v = strong;
            }
            break;
        }
        case PLUM_OP_IS_TRAIT: {
            const PlumString *trait = plum_as_string(chunk->constants.items[READ_U16()]);
            PlumValue v = pop(vm);
            bool is = false;
            if (v.type == PLUM_VAL_OBJ && v.as.obj->type == PLUM_OBJ_OBJECT) {
                const PlumTreeInfo *tree = plum_as_object(v)->tree;
                for (int i = 0; i < tree->trait_count && !is; i++) {
                    is = strcmp(tree->traits[i], trait->chars) == 0;
                }
            }
            push(vm, plum_bool(is));
            plum_release(v);
            break;
        }
        case PLUM_OP_INVOKE:
        case PLUM_OP_INVOKE_FROM: {
            const PlumTreeInfo *tree = NULL;
            if (op == PLUM_OP_INVOKE_FROM) {
                tree = plum_as_tree_info(chunk->constants.items[READ_U16()]);
            }
            uint16_t id = READ_U16();
            uint16_t argc = READ_U16();
            if (tree == NULL) {
                tree = plum_as_object(*peek(vm, argc))->tree;
            }
            PlumFunction *fn = method_of(tree, id);
            SAVE_IP();
            if (fn == NULL) {
                FAIL(PLUM_ERROR_ERROR, "internal error: %s has no method %d", tree->name, (int)id);
            }
            if (!invoke(vm, fn, argc)) {
                goto thrown;
            }
            frame = &vm->frames[vm->frame_count - 1];
            ip = frame->ip;
            chunk = frame->fn->chunk;
            break;
        }
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
        case PLUM_OP_JUMP_IF_NONE_KEEP: {
            uint16_t d = READ_U16();
            if (peek(vm, 0)->type == PLUM_VAL_NONE) {
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
            PlumValue callee = *peek(vm, argc);
            PlumClosure *closure =
                callee.as.obj->type == PLUM_OBJ_CLOSURE ? plum_as_closure(callee) : NULL;
            PlumFunction *fn = closure != NULL ? closure->fn : plum_as_function(callee);
            SAVE_IP();
            if (!call(vm, fn, closure, argc)) {
                goto thrown;
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
                goto thrown;
            }
            if (vm->stopped) {
                return true; /* exit() */
            }
            break;
        }
        case PLUM_OP_TRY: {
            uint16_t d = READ_U16();
            if (vm->handler_count == vm->handler_cap) {
                vm->handler_cap = vm->handler_cap < 8 ? 8 : vm->handler_cap * 2;
                vm->handlers =
                    plum_realloc_array(vm->handlers, (size_t)vm->handler_cap, sizeof(Handler));
            }
            vm->handlers[vm->handler_count++] = (Handler){vm->frame_count, vm->top, ip + d};
            break;
        }
        case PLUM_OP_END_TRY:
            vm->handler_count -= READ_U16();
            break;
        case PLUM_OP_THROW:
            SAVE_IP();
            throw_error(vm, pop(vm));
            goto thrown;
        case PLUM_OP_RETURN:
        case PLUM_OP_RETURN_NONE: {
            PlumValue result = op == PLUM_OP_RETURN ? pop(vm) : plum_none();
            size_t base = frame->base;
            vm->frame_count--;
            while (vm->handler_count > 0 &&
                   vm->handlers[vm->handler_count - 1].frame_count > vm->frame_count) {
                vm->handler_count--; /* tries the function returned out of */
            }
            unwind_to(vm, base - 1); /* the slots and the function itself */
            push(vm, result);
            if (vm->frame_count == vm->stop_at) {
                return true; /* back to whoever started this run */
            }
            frame = &vm->frames[vm->frame_count - 1];
            ip = frame->ip;
            chunk = frame->fn->chunk;
            break;
        }
        case PLUM_OPCODE_COUNT:
        default:
            FAIL(PLUM_ERROR_ERROR, "internal error: unknown instruction %d", (int)op);
        }
        continue;

    thrown:
        /* an error was thrown (or exit() called in a callback) */
        if (!catch_thrown(vm)) {
            return false;
        }
        frame = &vm->frames[vm->frame_count - 1];
        ip = frame->ip;
        chunk = frame->fn->chunk;
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

/*
 * --check-leaks, once the program's own references are gone: report the
 * tree objects still alive (they keep each other alive in cycles), by tree
 * and the line that made them. Then break those cycles, so the memory is
 * really given back. Returns the number of objects found.
 */
static size_t check_leaks(VM *vm) {
    typedef struct Group {
        const char *tree;
        uint32_t line;
        size_t count;
    } Group;
    PLUM_ARRAY(Group) groups = {0};
    PLUM_ARRAY(PlumObject *) alive = {0};
    for (size_t i = 0; i < vm->tracked.len; i++) {
        PlumObject *o = vm->tracked.items[i]->target;
        if (o == NULL) {
            continue;
        }
        plum_array_push(&alive, o);
        bool found = false;
        for (size_t g = 0; g < groups.len && !found; g++) {
            if (groups.items[g].tree == o->tree->name &&
                groups.items[g].line == vm->tracked.items[i]->line) {
                groups.items[g].count++;
                found = true;
            }
        }
        if (!found) {
            Group g = {o->tree->name, vm->tracked.items[i]->line, 1};
            plum_array_push(&groups, g);
        }
    }
    if (alive.len == 0) {
        fprintf(vm->err, "leak check: every object was freed\n");
    } else {
        fprintf(vm->err, "leak check: %zu object%s still alive in reference cycles:\n", alive.len,
                alive.len == 1 ? " is" : "s are");
        for (size_t g = 0; g < groups.len; g++) {
            fprintf(vm->err, "  %zu %s, made on line %u\n", groups.items[g].count,
                    groups.items[g].tree, (unsigned)groups.items[g].line);
        }
        fprintf(vm->err, "  hint: make one direction of each cycle weak, like 'parent: weak "
                         "Node? = none'\n");
    }
    /* Break the cycles: every leaked object lets go of its fields. */
    for (size_t i = 0; i < alive.len; i++) {
        alive.items[i]->obj.refcount++; /* held while its fields go */
    }
    for (size_t i = 0; i < alive.len; i++) {
        PlumObject *o = alive.items[i];
        for (int f = 0; f < o->tree->field_count; f++) {
            PlumValue v = o->fields[f];
            o->fields[f] = plum_none();
            plum_release(v);
        }
    }
    for (size_t i = 0; i < alive.len; i++) {
        plum_release(plum_obj(&alive.items[i]->obj));
    }
    size_t n = alive.len;
    plum_array_free(&alive);
    plum_array_free(&groups);
    return n;
}

int plum_vm_run(const PlumCompiled *program, const PlumSource *source,
                const PlumRunOptions *options, FILE *out, FILE *err) {
    VM *vm = plum_alloc_zeroed(1, sizeof *vm);
    vm->source = source;
    vm->out = out;
    vm->err = err;
    vm->global_count = program->global_count;
    plum_random_key(vm->seed);
    vm->globals = plum_alloc_zeroed(program->global_count > 0 ? (size_t)program->global_count : 1,
                                    sizeof(PlumValue));

    /* <main> sits at stack index 0, like any called function. */
    push(vm, plum_retain(plum_obj(&program->main->obj)));
    vm->errors = program->errors;
    vm->options = options;
    vm->to_str = program->to_str;
    vm->text_hook = (PlumTextHook){object_text, vm};
    vm->thrown = plum_none();
    vm->trace_of = plum_none();
    if (!(call(vm, program->main, NULL, 0) && execute(vm)) && !vm->stopped) {
        report_uncaught(vm);
    }
    fflush(out);

    unwind_to(vm, 0);
    for (int i = 0; i < vm->global_count; i++) {
        plum_release(vm->globals[i]);
    }
    plum_release(vm->thrown);
    plum_release(vm->trace_of);
    int code = vm->exit_code;
    if (options->check_leaks) {
        fflush(out);
        if (check_leaks(vm) > 0 && code == 0) {
            code = 1;
        }
    }
    for (size_t i = 0; i < vm->tracked.len; i++) {
        plum_release(plum_obj(&vm->tracked.items[i]->obj));
    }
    plum_array_free(&vm->tracked);
    plum_free(vm->globals);
    plum_free(vm->trace);
    plum_free(vm->handlers);
    plum_free(vm->stack);
    plum_free(vm);
    return code;
}
