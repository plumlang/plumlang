/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/vm.h"

#include "compiler/chunk.h"
#include "stdlib/native.h"
#include "stdlib/platform.h"
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

/*
 * A call in progress (#120: kept small, since every call writes one). The
 * arguments are where the caller pushed them: they become slots 0 and up
 * in place, and the other slots are pushed above them.
 */
typedef struct Frame {
    PlumFunction *fn;
    const uint8_t *ip;
    uint32_t base; /* index of slot 0 in the stack */
    /* 1: the function value sits at base - 1 and goes when the call
     * returns (a closure's captured variables are read through it); 0: the
     * caller didn't push it (CALL_GLOBAL, methods). */
    uint32_t callee : 1;
    /* how many times `return f(...)` restarted this call of f (TAIL_SELF),
     * for its stack trace */
    uint32_t tail_calls : 31;
} Frame;

/* A try in progress: where to go, and what to unwind to, on an error. */
typedef struct Handler {
    int frame_count;       /* the frames the try's function sits on */
    size_t top;            /* the stack height when the try began */
    const uint8_t *target; /* where the catches start, with the error pushed */
} Handler;

/* Frames live in segments of FRAME_SEG, so a Frame never moves once made
 * (the run loop keeps pointers to them across calls) and a task only has
 * the segments it uses. */
#define FRAME_SEG  64
#define FRAME_SEGS ((PLUM_MAX_FRAMES + FRAME_SEG - 1) / FRAME_SEG)

/* A sleeper's time, or a socket wait's time limit. */
typedef struct Timer {
    double when;
    uint64_t order; /* ties go to the earlier timer */
    uint64_t seq;   /* the task's timer_seq when set: a newer one replaces it */
    struct PlumTask *task;
} Timer;

/* Something a native module keeps for the whole run (plum_native_program_data). */
typedef struct ProgramData {
    const void *key;
    void *value;
    void (*free_fn)(void *);
} ProgramData;

typedef struct PlumVM {
    PlumValue *stack;
    size_t top;
    size_t capacity;
    Frame *segs[FRAME_SEGS];
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
    PlumTreeInfo *const *errors;  /* the built-in error trees */
    int to_str;                   /* the method number of to_str */
    int equals;                   /* of equals (lists of objects compare with it) */
    int compare;                  /* of compare (sorting objects calls it) */
    PlumFunction *const *methods; /* the program's methods, for CALL_METHOD */
    int method_count;
    const PlumRunOptions *options;
    uint64_t rng[4];                /* math.random: xoshiro256** */
    PLUM_ARRAY(PlumWeak *) tracked; /* --check-leaks: a weak box for every tree object */
    PlumTextHook text_hook;         /* objects become text through their to_str() */
    int text_depth;                 /* how deep the value being printed is (text_hook) */
    /* Tasks (spec 12): all run on this thread, one at a time. The VM's
     * stack, frames and handlers above are the running task's; the others
     * keep theirs in their PlumTask until they run again. */
    struct PlumTask *current;
    struct PlumTask *main_task;
    PLUM_ARRAY(struct PlumTask *) tasks;  /* every task not finished (holds a reference) */
    PLUM_ARRAY(struct PlumTask *) ready;  /* waiting for their turn, in order */
    PLUM_ARRAY(struct PlumTask *) failed; /* finished with an error (holds a reference) */
    PlumPoller *poller;                   /* the sockets tasks wait on */
    PLUM_ARRAY(Timer) timers;             /* a heap: the next time to wake someone first */
    bool yielded;                         /* the running task gave way: it waits, or sleeps */
    uint64_t timer_order;                 /* how many times a task has gone to sleep */
    PLUM_ARRAY(ProgramData) data;         /* what native modules keep for the whole run */
    /* The REPL: runs one entry after another, keeping the globals and the
     * tasks still running. An entry's main task that couldn't finish is
     * parked here until the session ends, since channels and other tasks
     * may still point at it. */
    bool session;
    PLUM_ARRAY(struct PlumTask *) parked;
    uint64_t steps;        /* calls and backward jumps so far (max_steps) */
    bool limited;          /* a step or memory limit to check (step) */
    double slept;          /* seconds spent waiting for sleeps so far (max_sleep) */
    PlumAllocStats *stats; /* the allocator's counts, if it counts */
} VM;

typedef PLUM_ARRAY(struct PlumTask *) TaskList;

typedef enum TaskState {
    TASK_NEW,   /* spawned, not started: its stack holds the function and arguments */
    TASK_READY, /* in the ready queue */
    TASK_RUNNING,
    TASK_BLOCKED,  /* waiting for a channel or another task */
    TASK_SLEEPING, /* until wake_at */
    TASK_IO,       /* until io_fd is ready, or io_deadline */
    TASK_DONE,
    TASK_FAILED,
} TaskState;

typedef struct PlumTask {
    PlumObj obj;
    TaskState state;
    /* its registers, while it isn't the one running */
    PlumValue *stack;
    size_t top;
    size_t capacity;
    Frame *segs[FRAME_SEGS];
    int frame_count;
    Handler *handlers;
    int handler_count;
    int handler_cap;
    int start_argc;     /* TASK_NEW */
    bool started;       /* its first call has been made */
    double wake_at;     /* while sleeping; 0 otherwise */
    uint64_t timer_seq; /* bumped by each new timer, and when woken: older ones are void */
    intptr_t io_fd;     /* TASK_IO: the socket */
    bool io_write;      /* TASK_IO: waiting to write, not read */
    bool io_woken;      /* woken because io_fd became ready */
    double io_deadline; /* the native call's time limit; 0: none */
    void *call_state;   /* a native call's scratch across its waits */
    void (*call_state_free)(void *);
    PlumValue result;
    PlumValue error;  /* TASK_FAILED */
    char *trace;      /* TASK_FAILED: where it failed */
    bool waited;      /* someone has seen its result or error */
    bool queued;      /* in the ready queue */
    size_t index;     /* in vm->tasks */
    TaskList waiters; /* tasks waiting for this one to end */
} PlumTask;

typedef struct PlumChannel {
    PlumObj obj;
    PlumValue *items; /* a ring of `len` values from `head` */
    size_t head;
    size_t len;
    size_t cap;
    int64_t limit; /* 0: no limit */
    bool closed;
    TaskList receivers; /* waiting for a value */
    TaskList senders;   /* waiting for room */
} PlumChannel;

/*
 * What a global slot holds until its `plant` line (or function) sets it: a
 * none no program can make (plum_none's payload is 0), so reading it is a
 * runtime error and never a value of the wrong type.
 */
static inline PlumValue unset(void) {
#if PLUM_NANBOX
    return plum_value_of_bits(PLUM_NB_SPECIAL | 1u);
#else
    PlumValue v = plum_none();
    v.as.i = 1;
    return v;
#endif
}

static inline bool is_unset(PlumValue v) {
#if PLUM_NANBOX
    return v.bits == (PLUM_NB_SPECIAL | 1u);
#else
    return v.type == PLUM_VAL_NONE && v.as.i == 1;
#endif
}

static inline Frame *FRAME(VM *vm, int i) {
    return &vm->segs[(unsigned)i / FRAME_SEG][(unsigned)i % FRAME_SEG];
}

/* The closure a frame runs: only a function with captured variables reads
 * it, and such a function is always called with its closure under slot 0. */
static inline PlumClosure *frame_closure(const VM *vm, const Frame *f) {
    return plum_as_closure(vm->stack[f->base - 1]);
}

/* ------------------------------------------------------------------ */
/* The stack                                                           */
/* ------------------------------------------------------------------ */

static void grow(VM *vm, size_t need) {
    if (need <= vm->capacity) {
        return;
    }
    size_t cap = vm->capacity < 32 ? 32 : vm->capacity; /* small: every task has one */
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
    if (plum_is_ref(vm->trace_of) && plum_as_obj(vm->trace_of) == plum_as_obj(error)) {
        return;
    }
    plum_release(vm->trace_of);
    vm->trace_of = plum_retain(error);
    vm->trace_len = 0;
    const PlumObject *o = plum_as_object(error);
    const PlumString *message = plum_as_string(o->fields[0]); /* every error's first field */
    const Frame *top = FRAME(vm, vm->frame_count - 1);
    trace_add(vm, "%s:%u: %s: %s\n", top->fn->path != NULL ? top->fn->path : vm->source->path,
              (unsigned)frame_line(top), o->tree->name, message->chars);
    /* Innermost calls first; a deep recursion shows its two ends. */
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        int from_top = vm->frame_count - 1 - i;
        if (from_top == 10 && i > 2) {
            /* frames i down to 2 are skipped; 1 and 0 are shown */
            trace_add(vm, "  ... %d more calls\n", i - 1);
            i = 1;
        }
        const Frame *f = FRAME(vm, i);
        char tail[64] = "";
        if (f->tail_calls > 0) {
            /* the calls `return f(...)` made in place of this one (spec 6.5) */
            snprintf(tail, sizeof tail, " (after %u tail call%s)", (unsigned)f->tail_calls,
                     f->tail_calls == 1 ? "" : "s");
        }
        if (f->fn->path != NULL && strcmp(f->fn->path, vm->source->path) != 0) {
            /* code from an imported module */
            trace_add(vm, "  in %s, line %u of %s%s\n", f->fn->name, (unsigned)frame_line(f),
                      f->fn->path, tail);
        } else {
            trace_add(vm, "  in %s, line %u%s\n", f->fn->name, (unsigned)frame_line(f), tail);
        }
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
    FRAME(vm, vm->frame_count - 1)->ip = h->target;
    return true;
}

/* ------------------------------------------------------------------ */
/* Integer arithmetic: overflow is an error, never wrap-around         */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
/* One instruction and a flag test, where the compiler has them. */
#define add_overflows(a, b, out) __builtin_add_overflow((a), (b), (out))
#define sub_overflows(a, b, out) __builtin_sub_overflow((a), (b), (out))
#define mul_overflows(a, b, out) __builtin_mul_overflow((a), (b), (out))
#else
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
#endif

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
/*
 * A 64-bit division is several times slower than a 32-bit one on common
 * processors, and most operands are small and positive: those take the
 * 32-bit path, where floor division and C's truncating one agree.
 */
static inline bool small_positive(int64_t a, int64_t b) {
    return a >= 0 && a <= (int64_t)UINT32_MAX && b > 0 && b <= (int64_t)UINT32_MAX;
}

static inline int64_t floor_div(int64_t a, int64_t b) {
    if (small_positive(a, b)) {
        return (int64_t)((uint32_t)a / (uint32_t)b);
    }
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static inline int64_t floor_mod(int64_t a, int64_t b) {
    if (b > 0 && (b & (b - 1)) == 0) {
        return a & (b - 1); /* a power of two: two's complement gives the floor remainder */
    }
    if (small_positive(a, b)) {
        return (int64_t)((uint32_t)a % (uint32_t)b);
    }
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
    switch (plum_value_type(v)) {
    case PLUM_VAL_NONE:
        return "none";
    case PLUM_VAL_BOOL:
        return "bool";
    case PLUM_VAL_INT:
        return "int";
    case PLUM_VAL_FLOAT:
        return "float";
    case PLUM_VAL_OBJ:
        switch ((PlumObjType)plum_as_obj(v)->type) {
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
        case PLUM_OBJ_INT: /* never here: plum_value_type says PLUM_VAL_INT */
            return "int";
        case PLUM_OBJ_WEAK:
            return "weak";
        case PLUM_OBJ_TASK:
            return "Task";
        case PLUM_OBJ_HANDLE:
            return plum_as_handle(v)->kind;
        case PLUM_OBJ_CHANNEL:
            return "Channel";
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
        if (plum_is_int(v)) {
            result = plum_retain(v); /* a boxed int holds a reference */
        } else if (plum_is_float(v)) {
            double f = plum_as_float(v);
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
        if (plum_is_float(v)) {
            result = v;
        } else if (plum_is_int(v)) {
            result = plum_float((double)plum_as_int(v));
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
        if (plum_as_int(args[0]) < 0 || plum_as_int(args[0]) > 255) {
            runtime_error(vm, PLUM_ERROR_VALUE, "byte(%lld) is out of range; a byte is 0 to 255",
                          (long long)plum_as_int(args[0]));
            return false;
        }
        result = args[0]; /* 0 to 255: inline */
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
        if (!plum_as_bool(args[0])) {
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
        vm->exit_code = argc > 0 ? (int)plum_as_int(args[0]) : 0;
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
            if (!check_index(vm, plum_as_int(keys[d]), list->len)) {
                return NULL;
            }
            place = &list->items[plum_as_int(keys[d])];
        }
    }
    return place;
}

static bool execute(VM *vm);
static bool call(VM *vm, PlumFunction *fn, int argc, uint32_t callee);

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
    PlumFunction *f =
        plum_as_obj(fn)->type == PLUM_OBJ_CLOSURE ? plum_as_closure(fn)->fn : plum_as_function(fn);
    int saved = vm->stop_at;
    vm->stop_at = vm->frame_count;
    vm->callback_depth++;
    bool ok = call(vm, f, argc, 1) && execute(vm) && !vm->stopped;
    vm->callback_depth--;
    vm->stop_at = saved;
    if (!ok) {
        return false;
    }
    *out = pop(vm);
    return true;
}

static PlumFunction *method_of(const PlumTreeInfo *tree, int id);

/*
 * a == b for values of a list type whose objects have equals (spec 8.5):
 * lists item by item, tree objects with the left one's equals. The checker
 * made sure every object here has an equals taking the other. Sets *same;
 * false if an equals threw.
 */
static bool equal_by_equals(VM *vm, PlumValue a, PlumValue b, bool *same) {
    if (plum_is_obj(a, PLUM_OBJ_LIST) && plum_is_obj(b, PLUM_OBJ_LIST)) {
        /* the caller holds the outer lists, so these can't change under us */
        const PlumList *x = plum_as_list(a), *y = plum_as_list(b);
        *same = x->len == y->len;
        for (size_t i = 0; *same && i < x->len; i++) {
            if (!equal_by_equals(vm, x->items[i], y->items[i], same)) {
                return false;
            }
        }
        return true;
    }
    if (plum_is_obj(a, PLUM_OBJ_OBJECT) && plum_is_obj(b, PLUM_OBJ_OBJECT)) {
        PlumFunction *fn = method_of(plum_as_object(a)->tree, vm->equals);
        if (fn != NULL) {
            PlumValue pair[2] = {a, b};
            PlumValue r;
            if (!call_value(vm, plum_obj(&fn->obj), pair, 2, &r)) {
                return false;
            }
            *same = plum_as_bool(r);
            return true;
        }
    }
    *same = plum_values_equal(a, b);
    return true;
}

/* Does sort() or sorted() on this list order objects, by their compare()? */
static bool sorts_objects(PlumMethod m, PlumValue list) {
    if (m != PLUM_METHOD_LIST_SORT && m != PLUM_METHOD_LIST_SORTED) {
        return false;
    }
    const PlumList *l = plum_as_list(list);
    return l->len > 0 && plum_is_obj(l->items[0], PLUM_OBJ_OBJECT);
}

/*
 * A sorted copy of a list of objects, ordered by their compare(): a stable
 * merge sort. It sorts its own copy of the items, which compare() can't
 * reach, so whatever compare() does to the list doesn't matter here.
 */
static bool sort_objects(VM *vm, const PlumList *list, PlumList **out) {
    size_t n = list->len;
    PlumList *items = plum_list_new(n);
    for (size_t i = 0; i < n; i++) {
        plum_list_push(items, plum_retain(list->items[i]));
    }
    size_t *order = plum_alloc_zeroed(n > 0 ? n : 1, sizeof(size_t));
    size_t *tmp = plum_alloc_zeroed(n > 0 ? n : 1, sizeof(size_t));
    for (size_t i = 0; i < n; i++) {
        order[i] = i;
    }
    bool ok = true;
    for (size_t width = 1; ok && width < n; width *= 2) {
        for (size_t lo = 0; ok && lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                PlumValue pair[2] = {items->items[order[i]], items->items[order[j]]};
                PlumFunction *fn = method_of(plum_as_object(pair[0])->tree, vm->compare);
                PlumValue r;
                if (fn == NULL) {
                    runtime_error(vm, PLUM_ERROR_ERROR, "internal error: %s has no compare",
                                  plum_as_object(pair[0])->tree->name);
                    ok = false;
                    break;
                }
                if (!call_value(vm, plum_obj(&fn->obj), pair, 2, &r)) {
                    ok = false;
                    break;
                }
                /* <= keeps equal items in their original order */
                tmp[k++] = plum_as_int(r) <= 0 ? order[i++] : order[j++];
                plum_release_int(r);
            }
            while (ok && i < mid) {
                tmp[k++] = order[i++];
            }
            while (ok && j < hi) {
                tmp[k++] = order[j++];
            }
        }
        if (ok) {
            memcpy(order, tmp, n * sizeof(size_t));
        }
    }
    if (ok) {
        *out = plum_list_new(n);
        for (size_t i = 0; i < n; i++) {
            plum_list_push(*out, plum_retain(items->items[order[i]]));
        }
    }
    plum_free(order);
    plum_free(tmp);
    plum_release(plum_obj(&items->obj));
    return ok;
}

/*
 * map, filter, reduce, any, all, find and sorted_by, and the lookups by
 * equals (contains, index_of, ==) on lists of objects. They hold their own
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
            } else if (plum_as_bool(r)) {
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
            if (plum_as_bool(r) == want) {
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
    case PLUM_METHOD_LIST_CONTAINS_EQUALS:
    case PLUM_METHOD_LIST_INDEX_OF_EQUALS: {
        bool contains = m == PLUM_METHOD_LIST_CONTAINS_EQUALS;
        for (size_t i = 0; i < list->len; i++) {
            bool same;
            if (!equal_by_equals(vm, list->items[i], args[0], &same)) {
                return false;
            }
            if (same) {
                *result = contains ? plum_bool(true) : plum_int((int64_t)i);
                return true;
            }
        }
        *result = contains ? plum_bool(false) : plum_none();
        return true;
    }
    case PLUM_METHOD_LIST_EQUAL_ITEMS: {
        bool same;
        if (!equal_by_equals(vm, plum_obj(&list->obj), args[0], &same)) {
            return false;
        }
        *result = plum_bool(same);
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* What native functions (src/stdlib) can ask of the running program   */
/* ------------------------------------------------------------------ */

#define VM_OF(n) ((VM *)(void *)(n))

bool plum_native_throw(PlumNative *n, PlumErrorKind kind, const char *fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    runtime_error(VM_OF(n), kind, "%s", message);
    return false;
}

FILE *plum_native_out(PlumNative *n) {
    return VM_OF(n)->out;
}

FILE *plum_native_err(PlumNative *n) {
    return VM_OF(n)->err;
}

PlumString *plum_native_text(PlumNative *n, PlumValue v) {
    char error[256];
    return plum_value_text_with(v, NULL, &VM_OF(n)->text_hook, error, sizeof error);
}

static uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

uint64_t plum_native_random(PlumNative *n) {
    uint64_t *s = VM_OF(n)->rng;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

/* Fill the generator's state from one number (splitmix64), so a seed
 * gives the same numbers on every platform. */
void plum_native_seed_random(PlumNative *n, uint64_t seed) {
    uint64_t *s = VM_OF(n)->rng;
    for (int i = 0; i < 4; i++) {
        uint64_t z = (seed += 0x9E3779B97F4A7C15u);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
        s[i] = z ^ (z >> 31);
    }
}

struct PlumTable *plum_native_new_map(PlumNative *n) {
    return plum_table_new(false, VM_OF(n)->seed);
}

int plum_native_argc(PlumNative *n) {
    return VM_OF(n)->options->argc;
}

const char *plum_native_arg(PlumNative *n, int i) {
    const PlumRunOptions *o = VM_OF(n)->options;
    return i >= 0 && i < o->argc ? o->argv[i] : "";
}

void **plum_native_program_data(PlumNative *n, const void *key, void (*free_fn)(void *)) {
    VM *vm = VM_OF(n);
    for (size_t i = 0; i < vm->data.len; i++) {
        if (vm->data.items[i].key == key) {
            return &vm->data.items[i].value;
        }
    }
    plum_array_push(&vm->data, ((ProgramData){key, NULL, free_fn}));
    return &vm->data.items[vm->data.len - 1].value;
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

static inline Frame *call_frame(VM *vm, PlumFunction *fn, int argc, uint32_t callee);

/*
 * Call `fn` as a method: the object sits under its `argc` arguments and
 * becomes slot 0 (self). Nothing goes under it: the program holds its
 * methods for the whole run, and the result takes the object's place.
 * Returns the new frame, or NULL if the call failed.
 */
static inline Frame *invoke(VM *vm, PlumFunction *fn, int argc) {
    return call_frame(vm, fn, argc + 1, 0);
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */

static PlumTask *as_task(PlumValue v) {
    return (PlumTask *)(void *)plum_as_obj(v);
}

static PlumChannel *as_channel(PlumValue v) {
    return (PlumChannel *)(void *)plum_as_obj(v);
}

/* Move the running registers into `t`, leaving the VM's empty. */
static void save_task(VM *vm, PlumTask *t) {
    t->stack = vm->stack;
    t->top = vm->top;
    t->capacity = vm->capacity;
    memcpy(t->segs, vm->segs, sizeof t->segs);
    t->frame_count = vm->frame_count;
    t->handlers = vm->handlers;
    t->handler_count = vm->handler_count;
    t->handler_cap = vm->handler_cap;
    vm->stack = NULL;
    vm->top = 0;
    vm->capacity = 0;
    memset(vm->segs, 0, sizeof vm->segs);
    vm->frame_count = 0;
    vm->handlers = NULL;
    vm->handler_count = 0;
    vm->handler_cap = 0;
}

/* Make `t`'s registers the running ones. */
static void load_task(VM *vm, PlumTask *t) {
    vm->stack = t->stack;
    vm->top = t->top;
    vm->capacity = t->capacity;
    memcpy(vm->segs, t->segs, sizeof vm->segs);
    vm->frame_count = t->frame_count;
    vm->handlers = t->handlers;
    vm->handler_count = t->handler_count;
    vm->handler_cap = t->handler_cap;
    t->stack = NULL;
    t->top = 0;
    t->capacity = 0;
    memset(t->segs, 0, sizeof t->segs);
    t->frame_count = 0;
    t->handlers = NULL;
    t->handler_count = 0;
    t->handler_cap = 0;
    vm->current = t;
}

static PlumTask *task_new(VM *vm) {
    PlumTask *t = plum_alloc_zeroed(1, sizeof *t);
    t->obj.refcount = 1; /* vm->tasks holds it */
    t->obj.type = PLUM_OBJ_TASK;
    plum_alloc_count(PLUM_MADE_TASK);
    t->result = plum_none();
    t->error = plum_none();
    t->index = vm->tasks.len;
    plum_array_push(&vm->tasks, t);
    return t;
}

static bool task_ended(const PlumTask *t) {
    return t->state == TASK_DONE || t->state == TASK_FAILED;
}

/* Put `t` in line to run (once). */
static void make_ready(VM *vm, PlumTask *t) {
    if (!t->queued && !task_ended(t)) {
        t->timer_seq++; /* any timer it had is void now */
        t->state = TASK_READY;
        t->queued = true;
        plum_array_push(&vm->ready, t);
    }
}

/* SPAWN: a task for the function and `argc` arguments on top of the stack. */
static PlumTask *spawn(VM *vm, int argc) {
    PlumTask *t = task_new(vm);
    size_t n = (size_t)argc + 1;
    t->capacity = n < 16 ? 16 : n;
    t->stack = plum_alloc_zeroed(t->capacity, sizeof(PlumValue));
    memcpy(t->stack, &vm->stack[vm->top - n], n * sizeof(PlumValue)); /* the refs move */
    vm->top -= n;
    t->top = n;
    t->start_argc = argc;
    make_ready(vm, t);
    t->obj.refcount++; /* and the program's reference */
    return t;
}

/* The task has ended: wake whoever waits, and stop tracking it. */
static void task_finished(VM *vm, PlumTask *t) {
    for (size_t i = 0; i < t->waiters.len; i++) {
        make_ready(vm, t->waiters.items[i]);
    }
    t->waiters.len = 0;
    PlumTask *last = vm->tasks.items[--vm->tasks.len]; /* out of vm->tasks */
    vm->tasks.items[t->index] = last;
    last->index = t->index;
    if (t->state == TASK_FAILED && t != vm->main_task) {
        plum_array_push(&vm->failed, t); /* the reference moves */
    } else {
        plum_release(plum_obj(&t->obj));
    }
}

void plum_task_destroy(PlumObj *obj) {
    PlumTask *t = (PlumTask *)(void *)obj;
    for (size_t i = 0; i < t->top; i++) {
        plum_release(t->stack[i]);
    }
    plum_free(t->stack);
    for (int i = 0; i < FRAME_SEGS; i++) {
        plum_free(t->segs[i]);
    }
    plum_free(t->handlers);
    if (t->call_state != NULL && t->call_state_free != NULL) {
        t->call_state_free(t->call_state);
    }
    plum_release(t->result);
    plum_release(t->error);
    plum_free(t->trace);
    plum_array_free(&t->waiters);
    plum_free(t);
}

void plum_channel_destroy(PlumObj *obj) {
    PlumChannel *ch = (PlumChannel *)(void *)obj;
    for (size_t i = 0; i < ch->len; i++) {
        plum_release(ch->items[(ch->head + i) % ch->cap]);
    }
    plum_free(ch->items);
    plum_array_free(&ch->receivers);
    plum_array_free(&ch->senders);
    plum_free(ch);
}

/* The running task gives way; its state is already set (blocked or sleeping). */
static bool give_way(VM *vm) {
    vm->yielded = true;
    return false;
}

/* A task can only wait when nothing but itself is on the C stack. */
static bool can_wait(VM *vm, const char *what) {
    if (vm->callback_depth == 0) {
        return true;
    }
    runtime_error(vm, PLUM_ERROR_ERROR,
                  "%s can't wait inside to_str, map or another callback; wait outside it", what);
    return false;
}

static void add_timer(VM *vm, PlumTask *t, double when);

/*
 * A sandboxed run may spend max_sleep seconds in all waiting for sleeps
 * (tasks sleeping at the same time wait together): may it sleep
 * `seconds` more? If not, the program stops, as if by exit(1).
 */
static bool sleep_allowed(VM *vm, double seconds) {
    const PlumRunOptions *o = vm->options;
    if (!o->sandbox || vm->slept + seconds <= o->max_sleep) {
        return true;
    }
    fprintf(vm->err,
            "%s: Error: stopped sleeping more than %g seconds in all, the limit of this run\n",
            vm->source->path, o->max_sleep);
    vm->exit_code = 1;
    vm->stopped = true;
    return false;
}

bool plum_native_sleep(PlumNative *n, double seconds) {
    VM *vm = VM_OF(n);
    PlumTask *t = vm->current;
    if ((vm->callback_depth > 0 || t == NULL || t->wake_at == 0) && !sleep_allowed(vm, seconds)) {
        return false;
    }
    if (vm->callback_depth > 0 || t == NULL) {
        plum_os_sleep(seconds);
        vm->slept += seconds;
        return true;
    }
    double now = plum_os_monotonic();
    if (t->wake_at == 0) {
        t->wake_at = now + seconds; /* the first call: start sleeping */
        if (t->wake_at == 0) {
            t->wake_at = 1e-9;
        }
        t->state = TASK_SLEEPING;
        add_timer(vm, t, t->wake_at);
        return give_way(vm);
    }
    if (now >= t->wake_at) {
        t->wake_at = 0; /* the call made again once the time is up: done */
        return true;
    }
    t->state = TASK_SLEEPING;
    add_timer(vm, t, t->wake_at);
    return give_way(vm);
}

int plum_native_wait_io(PlumNative *n, intptr_t fd, bool write, double timeout) {
    VM *vm = VM_OF(n);
    PlumTask *t = vm->current;
    double now = plum_os_monotonic();
    if (vm->callback_depth > 0 || t == NULL) {
        /* inside a callback: just wait here */
        PlumPollItem item = {fd, write, false};
        int ms = timeout > 0 ? (int)(timeout * 1000.0 + 0.5) : -1;
        return plum_os_poll(&item, 1, ms) > 0 ? 1 : -1;
    }
    if (t->io_woken && t->io_fd == fd && t->io_write == write) {
        t->io_woken = false; /* the socket is ready: go ahead */
        return 1;
    }
    t->io_woken = false;
    if (t->io_deadline == 0 && timeout > 0) {
        t->io_deadline = now + timeout;
    }
    if (t->io_deadline > 0 && now >= t->io_deadline) {
        return -1;
    }
    t->io_fd = fd;
    t->io_write = write;
    t->state = TASK_IO;
    plum_poller_arm(vm->poller, fd, write, t);
    if (t->io_deadline > 0) {
        add_timer(vm, t, t->io_deadline);
    }
    give_way(vm);
    return 0;
}

void **plum_native_call_state(PlumNative *n, void (*free_fn)(void *)) {
    PlumTask *t = VM_OF(n)->current;
    t->call_state_free = free_fn;
    return &t->call_state;
}

/* A native call has ended (not just paused): drop what it kept. */
static void end_native_call(VM *vm) {
    PlumTask *t = vm->current;
    if (t == NULL || vm->callback_depth > 0) {
        return;
    }
    if (t->call_state != NULL && t->call_state_free != NULL) {
        t->call_state_free(t->call_state);
    }
    t->call_state = NULL;
    t->call_state_free = NULL;
    t->io_deadline = 0;
    t->io_woken = false;
}

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* task.wait(): the task's result, or its error thrown here. */
static bool t_wait(ARGS) {
    (void)argc;
    VM *vm = VM_OF(n);
    PlumTask *t = as_task(args[0]);
    if (t->state == TASK_DONE) {
        t->waited = true;
        *result = plum_retain(t->result);
        return true;
    }
    if (t->state == TASK_FAILED) {
        t->waited = true;
        /* thrown again here, keeping the trace of where it failed */
        vm->trace_len = 0;
        if (vm->trace != NULL) {
            vm->trace[0] = '\0';
        }
        if (t->trace != NULL) {
            trace_add(vm, "%s", t->trace);
        }
        plum_release(vm->trace_of);
        vm->trace_of = plum_retain(t->error);
        throw_error(vm, plum_retain(t->error));
        return false;
    }
    if (t == vm->current) {
        return plum_native_throw(n, PLUM_ERROR_ERROR, "a task can't wait for itself");
    }
    if (!can_wait(vm, "wait()")) {
        return false;
    }
    plum_array_push(&t->waiters, vm->current);
    vm->current->state = TASK_BLOCKED;
    return give_way(vm);
}

static bool t_done(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(task_ended(as_task(args[0])));
    return true;
}

static bool c_new(ARGS) {
    int64_t limit = argc > 0 && plum_is_int(args[0]) ? plum_as_int(args[0]) : 0;
    if (limit < 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "Channel(%lld): the capacity must be 0 (no limit) or more",
                                 (long long)limit);
    }
    PlumChannel *ch = plum_alloc_zeroed(1, sizeof *ch);
    ch->obj.refcount = 1;
    ch->obj.type = PLUM_OBJ_CHANNEL;
    plum_alloc_count(PLUM_MADE_CHANNEL);
    ch->limit = limit;
    *result = plum_obj(&ch->obj);
    return true;
}

/* Wake the task that has waited longest on `list`. */
static void wake_first(VM *vm, TaskList *list) {
    if (list->len > 0) {
        PlumTask *t = list->items[0];
        memmove(list->items, list->items + 1, (list->len - 1) * sizeof(PlumTask *));
        list->len--;
        make_ready(vm, t);
    }
}

static bool c_send(ARGS) {
    (void)argc;
    VM *vm = VM_OF(n);
    PlumChannel *ch = as_channel(args[0]);
    if (ch->closed) {
        return plum_native_throw(n, PLUM_ERROR_ERROR, "send() on a closed channel");
    }
    if (ch->limit > 0 && ch->len >= (size_t)ch->limit) {
        if (!can_wait(vm, "send()")) {
            return false;
        }
        plum_array_push(&ch->senders, vm->current); /* wait for room */
        vm->current->state = TASK_BLOCKED;
        return give_way(vm);
    }
    if (ch->len == ch->cap) {
        size_t cap = ch->cap < 8 ? 8 : ch->cap * 2;
        PlumValue *items = plum_alloc_zeroed(cap, sizeof(PlumValue));
        for (size_t i = 0; i < ch->len; i++) {
            items[i] = ch->items[(ch->head + i) % ch->cap];
        }
        plum_free(ch->items);
        ch->items = items;
        ch->head = 0;
        ch->cap = cap;
    }
    ch->items[(ch->head + ch->len) % ch->cap] = plum_retain(args[1]);
    ch->len++;
    wake_first(vm, &ch->receivers);
    *result = plum_none();
    return true;
}

/* receive(): the next value; none once the channel is closed and empty. */
static bool c_receive(ARGS) {
    (void)argc;
    VM *vm = VM_OF(n);
    PlumChannel *ch = as_channel(args[0]);
    if (ch->len > 0) {
        *result = ch->items[ch->head]; /* the reference moves */
        ch->head = (ch->head + 1) % ch->cap;
        ch->len--;
        wake_first(vm, &ch->senders);
        return true;
    }
    if (ch->closed) {
        *result = plum_none();
        return true;
    }
    if (!can_wait(vm, "receive()")) {
        return false;
    }
    plum_array_push(&ch->receivers, vm->current);
    vm->current->state = TASK_BLOCKED;
    return give_way(vm);
}

static bool c_close(ARGS) {
    (void)argc;
    VM *vm = VM_OF(n);
    PlumChannel *ch = as_channel(args[0]);
    ch->closed = true;
    while (ch->receivers.len > 0) {
        wake_first(vm, &ch->receivers); /* they see the end */
    }
    while (ch->senders.len > 0) {
        wake_first(vm, &ch->senders); /* they see it closed */
    }
    *result = plum_none();
    return true;
}

#undef ARGS

/* Typed by the checker itself (Task[T] and Channel[T]); the signatures here
 * are for `plum stdlib`. */
static const PlumNativeFunction task_functions[] = {
    {"wait", "(self: Task[T]) -> T", t_wait,
     "the task's result once it ends; if it failed, its error is thrown here"},
    {"done", "(self: Task[T]) -> bool", t_done, "has the task ended?"},
    {"new_channel", "(capacity: int = 0) -> Channel[T]", c_new,
     "Channel[T](capacity): a channel; a capacity of 0 means no limit"},
    {"send", "(self: Channel[T], value: T)", c_send,
     "put a value in the channel; waits for room if it has a capacity and is full"},
    {"receive", "(self: Channel[T]) -> T?", c_receive,
     "the next value, waiting for one; none once the channel is closed and empty"},
    {"close", "(self: Channel[T])", c_close,
     "no more values: receivers get what's left, then none"},
};

const PlumNativeModule plum_module_tasks = {
    "task.methods", "the methods of Task and Channel (no seed needed)",
    task_functions, sizeof task_functions / sizeof task_functions[0],
    NULL,           0,
};

/*
 * Run tasks until the program's main task ends (or exit() is called): each
 * runs until it ends or waits, then the next in line. When none is ready,
 * the loop sleeps until the first sleeper's time. Returns false if every
 * task is waiting with nothing left to wake it (a deadlock).
 */
/* ------------------------------------------------------------------ */
/* Timers and sockets: what the scheduler waits for                    */
/* ------------------------------------------------------------------ */

/* Is a before b in the timer heap? Earlier first; equal times in order. */
static bool timer_before(const Timer *a, const Timer *b) {
    return a->when < b->when || (a->when == b->when && a->order < b->order);
}

/* Wake `t` at `when` (it holds a reference until the timer is used). */
static void add_timer(VM *vm, PlumTask *t, double when) {
    Timer tm = {when, vm->timer_order++, ++t->timer_seq, t};
    t->obj.refcount++;
    plum_array_push(&vm->timers, tm);
    size_t i = vm->timers.len - 1;
    while (i > 0) {
        size_t up = (i - 1) / 2;
        if (!timer_before(&vm->timers.items[i], &vm->timers.items[up])) {
            break;
        }
        Timer swap = vm->timers.items[i];
        vm->timers.items[i] = vm->timers.items[up];
        vm->timers.items[up] = swap;
        i = up;
    }
}

static Timer pop_timer(VM *vm) {
    Timer top = vm->timers.items[0];
    vm->timers.items[0] = vm->timers.items[--vm->timers.len];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, least = i;
        if (l < vm->timers.len && timer_before(&vm->timers.items[l], &vm->timers.items[least])) {
            least = l;
        }
        if (r < vm->timers.len && timer_before(&vm->timers.items[r], &vm->timers.items[least])) {
            least = r;
        }
        if (least == i) {
            break;
        }
        Timer swap = vm->timers.items[i];
        vm->timers.items[i] = vm->timers.items[least];
        vm->timers.items[least] = swap;
        i = least;
    }
    return top;
}

/* Is the timer still wanted: its task still asleep or waiting, for it? */
static bool timer_live(const Timer *tm) {
    PlumTask *t = tm->task;
    return tm->seq == t->timer_seq && (t->state == TASK_SLEEPING || t->state == TASK_IO);
}

/* Drop the timers nobody waits for any more from the top of the heap. */
static void prune_timers(VM *vm) {
    while (vm->timers.len > 0 && !timer_live(&vm->timers.items[0])) {
        Timer tm = pop_timer(vm);
        plum_release(plum_obj(&tm.task->obj));
    }
}

/*
 * Wait for something to wake a task: the next timer, or a socket. False
 * when there is nothing to wait for (every task waits on another).
 */
static bool wait_for_events(VM *vm) {
    prune_timers(vm);
    size_t armed = plum_poller_armed(vm->poller);
    if (vm->timers.len == 0 && armed == 0) {
        return false;
    }
    double now = plum_os_monotonic();
    int ms = -1;
    if (vm->timers.len > 0) {
        double left = vm->timers.items[0].when - now;
        ms = left <= 0 ? 0 : (int)(left * 1000.0) + 1;
    }
    if (armed > 0) {
        void *ready[256];
        int n = plum_poller_wait(vm->poller, ms, ready, 256);
        for (int i = 0; i < n; i++) {
            PlumTask *t = ready[i];
            if (t->state == TASK_IO) {
                t->io_woken = true;
                make_ready(vm, t); /* its socket is ready: it tries again */
            }
        }
    } else if (ms > 0) {
        plum_os_sleep(ms / 1000.0);
        vm->slept += ms / 1000.0; /* every task asleep waits at once */
    }
    /* the timers that are due, earliest first */
    now = plum_os_monotonic();
    while (vm->timers.len > 0 && vm->timers.items[0].when <= now) {
        Timer tm = pop_timer(vm);
        PlumTask *t = tm.task;
        if (timer_live(&tm)) {
            if (t->state == TASK_IO) {
                plum_poller_disarm(vm->poller, t->io_fd, t->io_write); /* its time is up */
            }
            make_ready(vm, t);
        }
        plum_release(plum_obj(&t->obj));
    }
    return true;
}

static bool run_tasks(VM *vm, PlumFunction *main_fn) {
    PlumTask *main = vm->main_task;
    (void)main_fn;
    size_t next = 0;
    for (;;) {
        if (next == vm->ready.len) {
            vm->ready.len = 0;
            next = 0;
            /* Nobody ready: wait for the next timer or a ready socket. */
            if (!wait_for_events(vm)) {
                return false; /* everyone waits for someone: deadlock */
            }
            continue;
        }
        PlumTask *t = vm->ready.items[next++];
        t->queued = false;
        load_task(vm, t);
        t->state = TASK_RUNNING;
        vm->yielded = false;
        bool ok;
        if (!t->started) {
            t->started = true;
            PlumValue fn = vm->stack[0];
            PlumFunction *f = plum_as_obj(fn)->type == PLUM_OBJ_CLOSURE ? plum_as_closure(fn)->fn
                                                                  : plum_as_function(fn);
            ok = call(vm, f, t->start_argc, 1) && execute(vm);
        } else {
            ok = execute(vm);
        }
        if (vm->stopped) {
            save_task(vm, t);
            return true; /* exit() */
        }
        if (!ok && vm->yielded) {
            save_task(vm, t); /* it waits or sleeps; its state says which */
            continue;
        }
        if (ok) {
            t->result = pop(vm);
            t->state = TASK_DONE;
        } else {
            /* an error nothing in the task caught */
            t->state = TASK_FAILED;
            t->error = vm->thrown;
            vm->thrown = plum_none();
            if (vm->trace != NULL) {
                size_t len = strlen(vm->trace);
                t->trace = plum_alloc(len + 1);
                memcpy(t->trace, vm->trace, len + 1);
            }
        }
        unwind_to(vm, 0);
        vm->frame_count = 0;
        vm->handler_count = 0;
        save_task(vm, t);
        if (t == main) {
            /* Who else is in line keeps their place: in the REPL, they
             * carry on when the next entry runs. */
            memmove(vm->ready.items, vm->ready.items + next,
                    (vm->ready.len - next) * sizeof *vm->ready.items);
            vm->ready.len -= next;
            return true;
        }
        task_finished(vm, t);
    }
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

/*
 * One more step of the run (a call or a backward jump): false, the program
 * stopped as if by exit(1), once it is past the limits of its options.
 */
static bool step_limited(VM *vm) {
    const PlumRunOptions *o = vm->options;
    if (o->max_steps > 0 && ++vm->steps > o->max_steps) {
        fprintf(vm->err, "%s: Error: stopped after %llu steps, the limit of this run\n",
                vm->source->path, (unsigned long long)o->max_steps);
    } else if (o->max_memory > 0 && vm->stats != NULL && vm->stats->bytes_in_use > o->max_memory) {
        fprintf(vm->err, "%s: Error: stopped using more than %lld bytes, the limit of this run\n",
                vm->source->path, (long long)o->max_memory);
    } else {
        return true;
    }
    vm->exit_code = 1;
    vm->stopped = true;
    return false;
}

/* Count a call or backward jump against the run's limits, if it has any. */
static inline bool step(VM *vm) {
    return !vm->limited || step_limited(vm);
}

/* The rare parts of a call: a step limit, the recursion limit, a new
 * segment of frames. False after an error. */
static bool call_slow(VM *vm) {
    if (!step(vm)) {
        return false;
    }
    if (vm->frame_count == PLUM_MAX_FRAMES) {
        runtime_error(vm, PLUM_ERROR_ERROR, "too much recursion: more than %d calls deep",
                      PLUM_MAX_FRAMES);
        return false;
    }
    if ((unsigned)vm->frame_count % FRAME_SEG == 0 &&
        vm->segs[(unsigned)vm->frame_count / FRAME_SEG] == NULL) {
        vm->segs[(unsigned)vm->frame_count / FRAME_SEG] =
            plum_alloc_zeroed(FRAME_SEG, sizeof(Frame));
    }
    return true;
}

/*
 * Start a call of `fn`, whose `argc` arguments are on top of the stack
 * (with the function value under them if `callee`). The arguments stay
 * where they are as slots 0 and up; the other slots are pushed as none.
 * Returns the new frame, or NULL after an error.
 */
static inline Frame *call_frame(VM *vm, PlumFunction *fn, int argc, uint32_t callee) {
    int n = vm->frame_count;
    if (vm->limited || (unsigned)n % FRAME_SEG == 0 || n == PLUM_MAX_FRAMES) {
        if (!call_slow(vm)) {
            return NULL;
        }
    }
    vm->frame_count = n + 1;
    Frame *f = FRAME(vm, n);
    f->fn = fn;
    f->ip = fn->chunk->code.items;
    f->base = (uint32_t)(vm->top - (size_t)argc);
    f->callee = callee & 1;
    f->tail_calls = 0;
    size_t need = vm->top + (size_t)(fn->slot_count - argc) + 1;
    if (need > vm->capacity) {
        grow(vm, need);
    }
    for (int i = argc; i < fn->slot_count; i++) {
        vm->stack[vm->top++] = plum_none();
    }
    return f;
}

static bool call(VM *vm, PlumFunction *fn, int argc, uint32_t callee) {
    return call_frame(vm, fn, argc, callee) != NULL;
}

/* Give up the reference the object value at `v` holds: inline while
 * something else still holds the object, plum_release when it may go. */
static inline void drop_ref(const PlumValue *v) {
    if (plum_as_obj(*v)->refcount > 1) {
        plum_as_obj(*v)->refcount--;
    } else {
        plum_release(*v);
    }
}

/* Release the objects among [from, to): the scan makes no calls, so a
 * return whose slots hold only numbers doesn't spill registers. */
#if defined(__GNUC__)
#define VM_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define VM_NOINLINE __declspec(noinline)
#else
#define VM_NOINLINE
#endif
VM_NOINLINE static void release_objects(PlumValue *from, PlumValue *to);

static inline void release_values(PlumValue *from, PlumValue *to) {
    for (PlumValue *v = from; v < to; v++) {
        if (plum_is_ref(*v)) {
            if (plum_as_obj(*v)->refcount > 1) {
                plum_as_obj(*v)->refcount--; /* most still have other holders */
            } else {
                release_objects(v, to); /* this one may go: the rest the slow way */
                return;
            }
        }
    }
}

VM_NOINLINE static void release_objects(PlumValue *from, PlumValue *to) {
    for (PlumValue *v = to; v-- > from;) {
        if (plum_is_ref(*v)) {
            drop_ref(v); /* most still have other holders */
        }
    }
}

/* Release everything from stack index `to` up. */
static void unwind_to(VM *vm, size_t to) {
    while (vm->top > to) {
        plum_release(pop(vm));
    }
}

/*
 * Copy a value field by field. In the wide layout the typed instructions
 * write only a value's number, in place; a whole-value (16-byte) read of
 * it right after would wait for that store to retire (a failed
 * store-to-load forward), which costs more than the instruction itself.
 * A NaN-boxed value is one word, written and read whole.
 */
static inline void copy_value(PlumValue *to, const PlumValue *from) {
#if PLUM_NANBOX
    *to = *from;
#else
    to->type = from->type;
    to->as = from->as;
#endif
}

/* Store the inline int `i` over the inline int at `p`: in the wide
 * layout, only the number. */
static inline void set_small(PlumValue *p, int64_t i) {
#if PLUM_NANBOX
    *p = plum_small_int(i);
#else
    p->as.i = i;
#endif
}

/* Make the int `i` the value at `p`, a new stack slot. */
static inline void push_int(PlumValue *p, int64_t i) {
#if PLUM_NANBOX
    *p = plum_int(i);
#else
    p->type = PLUM_VAL_INT;
    p->as.i = i;
#endif
}

/*
 * The slow paths of the int instructions, kept out of the run loop so its
 * fast paths stay small: a boxed operand or result (NaN boxing), or an
 * overflow error.
 */

/* A boxed int popped: its number, letting go of the box. */
VM_NOINLINE static int64_t boxed_int_popped(PlumValue v) {
    int64_t i = plum_as_int(v);
    plum_release_int(v);
    return i;
}

/* Pop an int, letting go of it if it was boxed. */
static inline int64_t pop_int(VM *vm) {
    PlumValue v = pop(vm);
    return plum_is_small_int(v) ? plum_small_int_of(v) : boxed_int_popped(v);
}

#define DROP_X 1 /* let go of x (in *to) if the operation fails */
#define DROP_Y 2 /* let go of y, whatever happens */

/*
 * *to = *to op y ('+', '-' or '*') for an int instruction whose fast path
 * didn't apply. On overflow reports the error (the caller saved ip) and
 * returns false, leaving *to as it was unless `drop` has DROP_X.
 */
VM_NOINLINE static bool int_arith_slow(VM *vm, int op, PlumValue *to, PlumValue y, int drop) {
    int64_t a = plum_as_int(*to), b = plum_as_int(y), r;
    bool overflow = op == '+' ? add_overflows(a, b, &r)
                    : op == '-' ? sub_overflows(a, b, &r)
                                : mul_overflows(a, b, &r);
    if (drop & DROP_Y) {
        plum_release_int(y);
    }
    if (overflow) {
        runtime_error(vm, PLUM_ERROR_OVERFLOW,
                      "integer overflow: %lld %c %lld doesn't fit in an int", (long long)a,
                      (char)op, (long long)b);
        if (drop & DROP_X) {
            plum_release_int(*to);
        }
        return false;
    }
    plum_set_int(to, r);
    return true;
}

/* Compare two ints, one of them boxed: <0, 0 or >0; lets go of what `drop`
 * says (DROP_X for x, DROP_Y for y). */
VM_NOINLINE static int int_compare_slow(PlumValue x, PlumValue y, int drop) {
    int64_t a = plum_as_int(x), b = plum_as_int(y);
    if (drop & DROP_X) {
        plum_release_int(x);
    }
    if (drop & DROP_Y) {
        plum_release_int(y);
    }
    return a < b ? -1 : a > b;
}

/* The numbers of two ints, one of them boxed, letting go of both. */
VM_NOINLINE static void ints_popped(PlumValue x, PlumValue y, int64_t *a, int64_t *b) {
    *a = plum_as_int(x);
    *b = plum_as_int(y);
    plum_release_int(x);
    plum_release_int(y);
}

/* Put `v` in the variable at `to`, giving up the value it held. */
static inline void store_value(PlumValue *to, const PlumValue *v) {
    if (plum_is_ref(*to)) {
        PlumValue old = *to;
        copy_value(to, v);
        plum_release(old);
    } else {
        copy_value(to, v);
    }
}

/* Make room to push one value. Growing may move the stack, so take any
 * pointer into it (a local's slot) after this. */
static inline void reserve_one(VM *vm) {
    if (vm->top == vm->capacity) {
        grow(vm, vm->top + 1);
    }
}

/* Push a copy of the variable at `from`, with a new reference, once
 * reserve_one made room. */
static inline void push_copy(VM *vm, const PlumValue *from) {
    PlumValue *to = &vm->stack[vm->top++];
    copy_value(to, from);
    if (plum_is_ref(*to)) {
        plum_as_obj(*to)->refcount++;
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
        p = &frame_closure(vm, frame)->cells[n]->value;
    }
    return p;
}

#define PLACE_KIND(op, local, global) ((op) == (local) ? 0 : (op) == (global) ? 1 : 2)

/* Can `m` run on `receiver` without making a list longer than PLUM_MAX_LIST?
 * If not, a ValueError is thrown. */
static bool list_has_room(VM *vm, PlumMethod m, PlumValue receiver) {
    if ((m == PLUM_METHOD_LIST_PUSH || m == PLUM_METHOD_LIST_INSERT) &&
        plum_as_list(receiver)->len >= PLUM_MAX_LIST) {
        runtime_error(vm, PLUM_ERROR_VALUE,
                      "%s(): the list has %zu items, the most a list can hold",
                      m == PLUM_METHOD_LIST_PUSH ? "push" : "insert", PLUM_MAX_LIST);
        return false;
    }
    return true;
}

/* Does `fn` reach outside the process? (PlumRunOptions.sandbox) */
static bool outside_world(const PlumNativeFunction *fn, const PlumRunOptions *options) {
    static const PlumNativeModule *const modules[] = {
        &plum_module_fs,         &plum_module_net, &plum_module_listener,
        &plum_module_connection, &plum_module_udp,
    };
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++) {
        if (fn >= modules[i]->functions &&
            fn < modules[i]->functions + modules[i]->function_count) {
            return true;
        }
    }
    return fn == plum_native_function(&plum_module_os, "run") ||
           (fn == plum_native_function(&plum_module_time, "sleep") && options->max_sleep <= 0);
}

static bool execute(VM *vm) {
    /* The running function's frame and its code. */
    Frame *frame;
    PlumValue ret; /* what a RETURN returns */
    const uint8_t *ip;
    PlumChunk *chunk;
#define ENTER_FRAME()                           \
    do {                                        \
        frame = FRAME(vm, vm->frame_count - 1); \
        ip = frame->ip;                         \
        chunk = frame->fn->chunk;               \
    } while (0)
    ENTER_FRAME();

#define READ_U16() read_u16(&ip)
#define SLOT(n)    (vm->stack[frame->base + (n)])
#define SAVE_IP()  (frame->ip = ip)
#define FAIL(...)                       \
    do {                                \
        SAVE_IP();                      \
        runtime_error(vm, __VA_ARGS__); \
        goto thrown;                    \
    } while (0)
/*
 * Typed binary operations work in place: the result takes the left
 * operand's slot, so there is no push and no capacity check.
 *
 * With NaN boxing (#121) an int is inline or, outside +-2^49, boxed. The
 * int instructions take a fast path when the operands and the result are
 * inline (always, in the wide layout), and otherwise read the operands
 * with plum_as_int, store with plum_set_int and let go of the boxed ints
 * they consumed. Both paths give the same results and the same errors.
 */
#define OVERFLOW(a, symbol, b)                                                                \
    FAIL(PLUM_ERROR_OVERFLOW, "integer overflow: %lld %s %lld doesn't fit in an int",       \
         (long long)(a), symbol, (long long)(b))
/* r = x fn y, when x and y are inline ints and so is the result. */
#define INT_FAST(fn, x, y, r)                                                                 \
    (plum_both_small_ints((x), (y)) && !fn(plum_small_int_of(x), plum_small_int_of(y), &(r)) && \
     plum_int_inline(r))
#define INT_BINARY(expr)                                      \
    do {                                                      \
        PlumValue *s_ = &vm->stack[--vm->top];                \
        int64_t a, b;                                         \
        if (plum_both_small_ints(s_[-1], s_[0])) {            \
            a = plum_small_int_of(s_[-1]);                    \
            b = plum_small_int_of(s_[0]);                     \
        } else {                                              \
            ints_popped(s_[-1], s_[0], &a, &b);               \
        }                                                     \
        s_[-1] = (expr);                                      \
    } while (0)
#define FLOAT_BINARY(expr)                                                 \
    do {                                                                   \
        PlumValue *s_ = &vm->stack[--vm->top];                             \
        double a = plum_as_float(s_[-1]), b = plum_as_float(s_[0]);        \
        s_[-1] = (expr);                                                   \
    } while (0)
#define CHECKED(fn, symbol)                                                \
    do {                                                                   \
        PlumValue *s_ = &vm->stack[--vm->top];                             \
        int64_t r_;                                                        \
        if (INT_FAST(fn, s_[-1], s_[0], r_)) {                             \
            set_small(&s_[-1], r_);                                        \
        } else {                                                           \
            SAVE_IP();                                                     \
            if (!int_arith_slow(vm, symbol[0], &s_[-1], s_[0], DROP_X | DROP_Y)) { \
                vm->top--;                                                 \
                goto thrown;                                               \
            }                                                              \
        }                                                                  \
    } while (0)
/* The _K and _L forms: the right operand is a constant or a local, not on
 * the stack, and the result replaces the left operand on top. */
#define KONST(n) (chunk->constants.items[(n)])
#define CHECKED_WITH(fn, symbol, right)                                    \
    do {                                                                   \
        uint16_t n_ = READ_U16();                                          \
        PlumValue *s_ = &vm->stack[vm->top - 1];                           \
        int64_t r_;                                                        \
        if (INT_FAST(fn, *s_, right(n_), r_)) {                            \
            set_small(s_, r_);                                             \
        } else {                                                           \
            SAVE_IP();                                                     \
            if (!int_arith_slow(vm, symbol[0], s_, right(n_), DROP_X)) {   \
                vm->top--;                                                 \
                goto thrown;                                               \
            }                                                              \
        }                                                                  \
    } while (0)
/* The LOCAL_ and GLOBAL_ forms change an int variable in place, with no
 * stack traffic; the _LK forms push local op constant. */
#define PLACE_CHECKED(place, check, fn, symbol, right)                     \
    do {                                                                   \
        uint16_t n_ = READ_U16();                                          \
        uint16_t m_ = READ_U16();                                          \
        check(n_);                                                         \
        PlumValue *v_ = &place(n_);                                        \
        int64_t r_;                                                        \
        if (INT_FAST(fn, *v_, right(m_), r_)) {                            \
            set_small(v_, r_);                                             \
        } else {                                                           \
            SAVE_IP();                                                     \
            if (!int_arith_slow(vm, symbol[0], v_, right(m_), 0)) {        \
                goto thrown;                                               \
            }                                                              \
        }                                                                  \
    } while (0)
/* The _FIELD forms: a field of the object in a local (self.count += 1),
 * changed in place. Objects are shared, so every holder sees it. */
#define FIELD_OF(n, f) (plum_as_object(SLOT(n))->fields[(f)])
#define FIELD_CHECKED(fn, symbol, right)                                   \
    do {                                                                   \
        uint16_t n_ = READ_U16();                                          \
        uint16_t f_ = READ_U16();                                          \
        uint16_t m_ = READ_U16();                                          \
        PlumValue *v_ = &FIELD_OF(n_, f_);                                 \
        int64_t r_;                                                        \
        if (INT_FAST(fn, *v_, right(m_), r_)) {                            \
            set_small(v_, r_);                                             \
        } else {                                                           \
            SAVE_IP();                                                     \
            if (!int_arith_slow(vm, symbol[0], v_, right(m_), 0)) {        \
                goto thrown;                                               \
            }                                                              \
        }                                                                  \
    } while (0)
#define FIELD_FLOAT(operator, right)                                                     \
    do {                                                                                 \
        uint16_t n_ = READ_U16();                                                        \
        uint16_t f_ = READ_U16();                                                        \
        uint16_t m_ = READ_U16();                                                        \
        PlumValue *v_ = &FIELD_OF(n_, f_);                                               \
        plum_set_float(v_, plum_as_float(*v_) operator plum_as_float(right(m_)));        \
    } while (0)
#define NO_CHECK(n)                       ((void)(n))
#define GLOBAL(n)                         (vm->globals[(n)])
#define LOCAL_CHECKED(fn, symbol, right)  PLACE_CHECKED(SLOT, NO_CHECK, fn, symbol, right)
#define GLOBAL_CHECKED(fn, symbol, right) PLACE_CHECKED(GLOBAL, CHECK_GLOBAL, fn, symbol, right)
#define PUSH_INT(value)                        \
    do {                                       \
        int64_t v_ = (value);                  \
        reserve_one(vm);                       \
        push_int(&vm->stack[vm->top++], v_);   \
    } while (0)
#define PUSH_CHECKED(fn, symbol)                         \
    do {                                                 \
        uint16_t n_ = READ_U16();                        \
        uint16_t k_ = READ_U16();                        \
        int64_t a = plum_as_int(SLOT(n_)), b = (int16_t)k_, r; \
        if (fn(a, b, &r)) {                              \
            OVERFLOW(a, symbol, b);                      \
        }                                                \
        PUSH_INT(r);                                     \
    } while (0)
#define FLOAT_WITH(operator, right)                                                   \
    do {                                                                              \
        uint16_t n_ = READ_U16();                                                     \
        PlumValue *s_ = &vm->stack[vm->top - 1];                                      \
        plum_set_float(s_, plum_as_float(*s_) operator plum_as_float(right(n_)));     \
    } while (0)
/* Compare the top two and jump unless the comparison holds; for floats,
 * a NaN makes every comparison but != false, so it jumps. */
#define JUMP_UNLESS_INT(operator)                                           \
    do {                                                                    \
        uint16_t d_ = READ_U16();                                           \
        PlumValue *s_ = &vm->stack[vm->top -= 2];                           \
        bool holds_;                                                        \
        if (plum_both_small_ints(s_[0], s_[1])) {                           \
            holds_ = plum_small_int_of(s_[0]) operator plum_small_int_of(s_[1]); \
        } else {                                                            \
            holds_ = int_compare_slow(s_[0], s_[1], DROP_X | DROP_Y) operator 0; \
        }                                                                   \
        if (!holds_) {                                                      \
            ip += d_;                                                       \
        }                                                                   \
    } while (0)
#define JUMP_UNLESS_FLOAT(operator)                                         \
    do {                                                                    \
        uint16_t d_ = READ_U16();                                           \
        PlumValue *s_ = &vm->stack[vm->top -= 2];                           \
        if (!(plum_as_float(s_[0]) operator plum_as_float(s_[1]))) {        \
            ip += d_;                                                       \
        }                                                                   \
    } while (0)
#define JUMP_UNLESS_INT_K(operator)                                         \
    do {                                                                    \
        uint16_t k_ = READ_U16();                                           \
        uint16_t d_ = READ_U16();                                           \
        PlumValue *s_ = &vm->stack[--vm->top];                              \
        PlumValue k_value_ = KONST(k_);                                     \
        bool holds_;                                                        \
        if (plum_both_small_ints(*s_, k_value_)) {                          \
            holds_ = plum_small_int_of(*s_) operator plum_small_int_of(k_value_); \
        } else {                                                            \
            holds_ = int_compare_slow(*s_, k_value_, DROP_X) operator 0;    \
        }                                                                   \
        if (!holds_) {                                                      \
            ip += d_;                                                       \
        }                                                                   \
    } while (0)
#define JUMP_UNLESS_FLOAT_K(operator)                                       \
    do {                                                                    \
        uint16_t k_ = READ_U16();                                           \
        uint16_t d_ = READ_U16();                                           \
        PlumValue *s_ = &vm->stack[--vm->top];                              \
        if (!(plum_as_float(*s_) operator plum_as_float(KONST(k_)))) {      \
            ip += d_;                                                       \
        }                                                                   \
    } while (0)
#define JUMP_UNLESS_LK(operator)                              \
    do {                                                      \
        uint16_t n_ = READ_U16();                             \
        uint16_t k_ = READ_U16();                             \
        uint16_t d_ = READ_U16();                             \
        if (!(plum_as_int(SLOT(n_)) operator(int16_t) k_)) {  \
            ip += d_;                                         \
        }                                                     \
    } while (0)
/* A module variable read before its `plant` line ran (spec 9.1). */
#define CHECK_GLOBAL(n)                                                                      \
    do {                                                                                     \
        if (is_unset(vm->globals[(n)])) {                                                    \
            FAIL(PLUM_ERROR_ERROR,                                                           \
                 "a module variable was used before its `plant` line ran: declare it above " \
                 "the code that calls this function");                                       \
        }                                                                                    \
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
        const uint8_t *op_start = ip; /* where a task that gives way picks up again */
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
        case PLUM_OP_GET_LOCAL: {
            uint16_t n = READ_U16();
            reserve_one(vm);
            push_copy(vm, &SLOT(n));
            break;
        }
        case PLUM_OP_SET_LOCAL: {
            uint16_t n = READ_U16();
            vm->top--;
            store_value(&SLOT(n), &vm->stack[vm->top]);
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
            push(vm, plum_retain(frame_closure(vm, frame)->cells[READ_U16()]->value));
            break;
        case PLUM_OP_SET_CAPTURE: {
            PlumCell *cell = frame_closure(vm, frame)->cells[READ_U16()];
            PlumValue v = pop(vm);
            plum_release(cell->value);
            cell->value = v;
            break;
        }
        case PLUM_OP_GET_SELF:
            /* A nested function: always called with itself under slot 0. */
            push(vm, plum_retain(vm->stack[frame->base - 1]));
            break;
        case PLUM_OP_MAKE_CLOSURE: {
            PlumFunction *fn = plum_as_function(chunk->constants.items[READ_U16()]);
            PlumClosure *closure = plum_closure_new(fn);
            for (int i = 0; i < fn->capture_count; i++) {
                const PlumCapture *cap = &fn->captures[i];
                PlumCell *cell = cap->from_local ? plum_as_cell(SLOT(cap->index))
                                                 : frame_closure(vm, frame)->cells[cap->index];
                cell->obj.refcount++;
                closure->cells[i] = cell;
            }
            push(vm, plum_obj(&closure->obj));
            break;
        }
        case PLUM_OP_GET_GLOBAL: {
            uint16_t n = READ_U16();
            CHECK_GLOBAL(n);
            reserve_one(vm);
            push_copy(vm, &vm->globals[n]);
            break;
        }
        case PLUM_OP_SET_GLOBAL: {
            uint16_t n = READ_U16();
            vm->top--;
            store_value(&vm->globals[n], &vm->stack[vm->top]);
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
            int64_t b = pop_int(vm);
            int64_t a = pop_int(vm);
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
            int64_t b = pop_int(vm);
            int64_t a = pop_int(vm);
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
            int64_t a = pop_int(vm);
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
            push(vm, plum_int(~pop_int(vm)));
            break;
        case PLUM_OP_SHL:
        case PLUM_OP_SHR: {
            int64_t b = pop_int(vm);
            int64_t a = pop_int(vm);
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
        case PLUM_OP_ADD_INT_K:
            CHECKED_WITH(add_overflows, "+", KONST);
            break;
        case PLUM_OP_SUB_INT_K:
            CHECKED_WITH(sub_overflows, "-", KONST);
            break;
        case PLUM_OP_MUL_INT_K:
            CHECKED_WITH(mul_overflows, "*", KONST);
            break;
        case PLUM_OP_FLOORDIV_INT_K: {
            /* the compiler only uses it for a constant above 0: no error is possible */
            PlumValue *s_ = &vm->stack[vm->top - 1];
            PlumValue k_ = KONST(READ_U16());
            if (plum_both_small_ints(*s_, k_)) { /* and so is the result */
                set_small(s_, floor_div(plum_small_int_of(*s_), plum_small_int_of(k_)));
            } else {
                plum_set_int(s_, floor_div(plum_as_int(*s_), plum_as_int(k_)));
            }
            break;
        }
        case PLUM_OP_MOD_INT_K: {
            PlumValue *s_ = &vm->stack[vm->top - 1];
            PlumValue k_ = KONST(READ_U16());
            if (plum_both_small_ints(*s_, k_)) {
                set_small(s_, floor_mod(plum_small_int_of(*s_), plum_small_int_of(k_)));
            } else {
                plum_set_int(s_, floor_mod(plum_as_int(*s_), plum_as_int(k_)));
            }
            break;
        }
        case PLUM_OP_LOCAL_ADD_INT_K:
            LOCAL_CHECKED(add_overflows, "+", KONST);
            break;
        case PLUM_OP_LOCAL_SUB_INT_K:
            LOCAL_CHECKED(sub_overflows, "-", KONST);
            break;
        case PLUM_OP_LOCAL_MUL_INT_K:
            LOCAL_CHECKED(mul_overflows, "*", KONST);
            break;
        case PLUM_OP_LOCAL_FLOORDIV_INT_K: {
            uint16_t n = READ_U16();
            uint16_t k = READ_U16();
            PlumValue *v = &SLOT(n);
            if (plum_both_small_ints(*v, KONST(k))) { /* above 0: the result is inline too */
                set_small(v, floor_div(plum_small_int_of(*v), plum_small_int_of(KONST(k))));
            } else {
                plum_set_int(v, floor_div(plum_as_int(*v), plum_as_int(KONST(k))));
            }
            break;
        }
        case PLUM_OP_LOCAL_MOD_INT_K: {
            uint16_t n = READ_U16();
            uint16_t k = READ_U16();
            PlumValue *v = &SLOT(n);
            if (plum_both_small_ints(*v, KONST(k))) {
                set_small(v, floor_mod(plum_small_int_of(*v), plum_small_int_of(KONST(k))));
            } else {
                plum_set_int(v, floor_mod(plum_as_int(*v), plum_as_int(KONST(k))));
            }
            break;
        }
        case PLUM_OP_GLOBAL_ADD_INT_K:
            GLOBAL_CHECKED(add_overflows, "+", KONST);
            break;
        case PLUM_OP_GLOBAL_SUB_INT_K:
            GLOBAL_CHECKED(sub_overflows, "-", KONST);
            break;
        case PLUM_OP_GLOBAL_ADD_INT_L:
            GLOBAL_CHECKED(add_overflows, "+", SLOT);
            break;
        case PLUM_OP_GLOBAL_SUB_INT_L:
            GLOBAL_CHECKED(sub_overflows, "-", SLOT);
            break;
        case PLUM_OP_ADD_INT_LK:
            PUSH_CHECKED(add_overflows, "+");
            break;
        case PLUM_OP_SUB_INT_LK:
            PUSH_CHECKED(sub_overflows, "-");
            break;
        case PLUM_OP_MUL_INT_LK:
            PUSH_CHECKED(mul_overflows, "*");
            break;
        case PLUM_OP_FLOORDIV_INT_LK: {
            uint16_t n = READ_U16();
            uint16_t k = READ_U16();
            PUSH_INT(floor_div(plum_as_int(SLOT(n)), (int16_t)k));
            break;
        }
        case PLUM_OP_MOD_INT_LK: {
            uint16_t n = READ_U16();
            uint16_t k = READ_U16();
            PUSH_INT(floor_mod(plum_as_int(SLOT(n)), (int16_t)k));
            break;
        }
        case PLUM_OP_LOCAL_ADD_INT_L:
            LOCAL_CHECKED(add_overflows, "+", SLOT);
            break;
        case PLUM_OP_LOCAL_SUB_INT_L:
            LOCAL_CHECKED(sub_overflows, "-", SLOT);
            break;
        case PLUM_OP_ADD_INT_L:
            CHECKED_WITH(add_overflows, "+", SLOT);
            break;
        case PLUM_OP_SUB_INT_L:
            CHECKED_WITH(sub_overflows, "-", SLOT);
            break;
        case PLUM_OP_MUL_INT_L:
            CHECKED_WITH(mul_overflows, "*", SLOT);
            break;

        case PLUM_OP_ADD_FLOAT:
            FLOAT_BINARY(plum_float_raw(a + b));
            break;
        case PLUM_OP_SUB_FLOAT:
            FLOAT_BINARY(plum_float_raw(a - b));
            break;
        case PLUM_OP_MUL_FLOAT:
            FLOAT_BINARY(plum_float_raw(a * b));
            break;
        case PLUM_OP_DIV_FLOAT:
            FLOAT_BINARY(plum_float_raw(a / b));
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
            push(vm, plum_float_raw(-plum_as_float(pop(vm))));
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
        case PLUM_OP_ADD_FLOAT_K:
            FLOAT_WITH(+, KONST);
            break;
        case PLUM_OP_SUB_FLOAT_K:
            FLOAT_WITH(-, KONST);
            break;
        case PLUM_OP_MUL_FLOAT_K:
            FLOAT_WITH(*, KONST);
            break;
        case PLUM_OP_DIV_FLOAT_K:
            FLOAT_WITH(/, KONST);
            break;
        case PLUM_OP_ADD_FLOAT_L:
            FLOAT_WITH(+, SLOT);
            break;
        case PLUM_OP_SUB_FLOAT_L:
            FLOAT_WITH(-, SLOT);
            break;
        case PLUM_OP_MUL_FLOAT_L:
            FLOAT_WITH(*, SLOT);
            break;
        case PLUM_OP_DIV_FLOAT_L:
            FLOAT_WITH(/, SLOT);
            break;
        case PLUM_OP_INT_TO_FLOAT:
            push(vm, plum_float_raw((double)pop_int(vm)));
            break;

        case PLUM_OP_CONCAT: {
            if (plum_as_string(*peek(vm, 1))->len >
                PLUM_MAX_STRING - plum_as_string(*peek(vm, 0))->len) {
                FAIL(PLUM_ERROR_VALUE,
                     "+: the text would be longer than %zu bytes, the most a "
                     "str can hold",
                     PLUM_MAX_STRING);
            }
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
            if (total > PLUM_MAX_STRING) {
                FAIL(PLUM_ERROR_VALUE,
                     "the text would be longer than %zu bytes, the most a "
                     "str can hold",
                     PLUM_MAX_STRING);
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

        case PLUM_OP_APPEND_LOCAL:
        case PLUM_OP_APPEND_GLOBAL: {
            uint16_t n = READ_U16();
            uint16_t count = READ_U16();
            if (op == PLUM_OP_APPEND_GLOBAL) {
                CHECK_GLOBAL(n);
            }
            PlumValue *place = op == PLUM_OP_APPEND_GLOBAL ? &vm->globals[n] : &SLOT(n);
            const PlumValue *parts = &vm->stack[vm->top - count];
            size_t total = plum_as_string(*place)->len;
            for (uint16_t i = 0; i < count; i++) {
                total = plum_add_size(total, plum_as_string(parts[i])->len);
            }
            if (total > PLUM_MAX_STRING) {
                FAIL(PLUM_ERROR_VALUE,
                     "+: the text would be longer than %zu bytes, the most a "
                     "str can hold",
                     PLUM_MAX_STRING);
            }
            plum_string_append(place, parts, count);
            unwind_to(vm, vm->top - count);
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
            int64_t i = pop_int(vm);
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
            int64_t i = pop_int(vm);
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
            int64_t hi = pop_int(vm);
            int64_t lo = pop_int(vm);
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
            if (plum_as_list(*peek(vm, 1))->len > PLUM_MAX_LIST - plum_as_list(*peek(vm, 0))->len) {
                FAIL(PLUM_ERROR_VALUE,
                     "+: the list would have more than %zu items, the most a "
                     "list can hold",
                     PLUM_MAX_LIST);
            }
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
            if (op == PLUM_OP_METHOD_GLOBAL) {
                CHECK_GLOBAL(slot);
            }
            PlumValue *receiver =
                op == PLUM_OP_METHOD
                    ? peek(vm, argc)
                    : place_of(vm, frame,
                               PLACE_KIND(op, PLUM_OP_METHOD_LOCAL, PLUM_OP_METHOD_GLOBAL), slot);
            PlumValue result;
            char error[256];
            if (sorts_objects((PlumMethod)m, *receiver)) {
                /* compare() runs: own the list, then off the stack */
                PlumValue list = plum_retain(*receiver);
                unwind_to(vm, vm->top - argc - (op == PLUM_OP_METHOD ? 1 : 0));
                SAVE_IP();
                PlumList *sorted = NULL;
                bool done = sort_objects(vm, plum_as_list(list), &sorted);
                plum_release(list);
                if (!done) {
                    goto thrown;
                }
                if (m == PLUM_METHOD_LIST_SORTED) {
                    push(vm, plum_obj(&sorted->obj));
                    break;
                }
                if (op == PLUM_OP_METHOD) {
                    plum_release(plum_obj(&sorted->obj)); /* sorting a temporary */
                } else {
                    /* the variable again (compare may have moved the stack or
                     * changed it): it holds the sorted list now */
                    receiver =
                        place_of(vm, frame,
                                 PLACE_KIND(op, PLUM_OP_METHOD_LOCAL, PLUM_OP_METHOD_GLOBAL), slot);
                    PlumValue old = *receiver;
                    *receiver = plum_obj(&sorted->obj);
                    plum_release(old);
                }
                push(vm, plum_none());
                break;
            }
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
            if (!list_has_room(vm, (PlumMethod)m, *receiver)) {
                SAVE_IP();
                goto thrown;
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
            if (op == PLUM_OP_SET_INDEX_GLOBAL) {
                CHECK_GLOBAL(slot);
            }
            PlumValue value = pop(vm);
            PlumValue *place = place_of(
                vm, frame, PLACE_KIND(op, PLUM_OP_SET_INDEX_LOCAL, PLUM_OP_SET_INDEX_GLOBAL), slot);
            if (plum_is_obj(*place, PLUM_OBJ_MAP)) {
                PlumValue key = pop(vm);
                plum_table_put(plum_table_unique(place), key, value); /* copy first if shared */
                break;
            }
            int64_t i = pop_int(vm);
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
            if (op == PLUM_OP_SET_PATH_GLOBAL) {
                CHECK_GLOBAL(slot);
            }
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
                if (!check_index(vm, plum_as_int(key), list->len)) {
                    plum_release(value);
                    goto thrown;
                }
                plum_release(list->items[plum_as_int(key)]);
                list->items[plum_as_int(key)] = value;
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
            if (op == PLUM_OP_METHOD_PATH_GLOBAL) {
                CHECK_GLOBAL(slot);
            }
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
            if (sorts_objects((PlumMethod)m, *receiver)) {
                /* m["k"].sort() on objects: compare() runs, then down the
                 * path again, since it may have moved the stack or changed
                 * what is there */
                PlumValue list = plum_retain(*receiver);
                PlumList *sorted = NULL;
                bool done = sort_objects(vm, plum_as_list(list), &sorted);
                plum_release(list);
                if (!done) {
                    goto thrown;
                }
                keys = &vm->stack[vm->top - (size_t)argc - (size_t)depth];
                root = under ? &plum_as_object(keys[-1])->fields[slot]
                             : place_of(vm, frame,
                                        PLACE_KIND(op, PLUM_OP_METHOD_PATH_LOCAL,
                                                   PLUM_OP_METHOD_PATH_GLOBAL),
                                        slot);
                receiver = walk_path(vm, root, keys, depth);
                if (receiver == NULL) {
                    plum_release(plum_obj(&sorted->obj));
                    goto thrown;
                }
                if (m == PLUM_METHOD_LIST_SORTED) {
                    result = plum_obj(&sorted->obj);
                } else {
                    PlumValue old = *receiver;
                    *receiver = plum_obj(&sorted->obj);
                    plum_release(old);
                    result = plum_none();
                }
                unwind_to(vm, vm->top - (size_t)argc - (size_t)depth - under);
                push(vm, result);
                break;
            }
            if (!list_has_room(vm, (PlumMethod)m, *receiver)) {
                SAVE_IP();
                goto thrown;
            }
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
            PlumObjType kind = (PlumObjType)plum_as_obj(SLOT(at))->type;
            if (kind != PLUM_OBJ_STRING && kind != PLUM_OBJ_MAP) { /* a list: the usual case */
                PlumList *list = plum_as_list(SLOT(at));
                /* positions and offsets are below 2^50: inline */
                int64_t pos = plum_small_int_of(SLOT(at + 1));
                if ((uint64_t)pos >= list->len) {
                    ip += d;
                    break;
                }
                set_small(&SLOT(at + 1), pos + 1);
                if (op == PLUM_OP_FOR_EACH2) {
                    SLOT(var) = plum_int(pos); /* ints hold no reference */
                    var++;
                }
                /* the item before is most likely still in the list */
                PlumValue *slot = &SLOT(var);
                if (plum_is_ref(*slot)) {
                    drop_ref(slot);
                }
                *slot = plum_retain(list->items[pos]);
                break;
            }
            if (kind == PLUM_OBJ_STRING) {
                const PlumString *s = plum_as_string(SLOT(at));
                size_t from = (size_t)plum_small_int_of(SLOT(at + 1)); /* a byte offset */
                if (from >= s->len) {
                    ip += d;
                    break;
                }
                size_t to = from + 1;
                while (to < s->len && ((unsigned char)s->chars[to] & 0xC0) == 0x80) {
                    to++;
                }
                set_small(&SLOT(at + 1), (int64_t)to);
                plum_release(SLOT(var));
                SLOT(var) = plum_obj(&plum_string_new(s->chars + from, to - from)->obj);
                break;
            }
            {
                /* maps give keys (and values with two names); sets give items */
                int64_t pos = plum_small_int_of(SLOT(at + 1));
                const PlumTableEntry *e = plum_table_next(plum_as_table(SLOT(at)), &pos);
                if (e == NULL) {
                    ip += d;
                    break;
                }
                set_small(&SLOT(at + 1), pos);
                plum_release(SLOT(var));
                SLOT(var) = plum_retain(e->key);
                if (op == PLUM_OP_FOR_EACH2) {
                    plum_release(SLOT(var + 1));
                    SLOT(var + 1) = plum_retain(e->value);
                }
                break;
            }
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
            push(vm, plum_bool(!plum_as_bool(pop(vm))));
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
        case PLUM_OP_GET_FIELD_L: {
            uint16_t n = READ_U16();
            uint16_t i = READ_U16();
            reserve_one(vm);
            push_copy(vm, &FIELD_OF(n, i));
            break;
        }
        case PLUM_OP_SET_FIELD_L: {
            uint16_t n = READ_U16();
            uint16_t i = READ_U16();
            vm->top--;
            store_value(&FIELD_OF(n, i), &vm->stack[vm->top]);
            break;
        }
        case PLUM_OP_FIELD_ADD_INT_K:
            FIELD_CHECKED(add_overflows, "+", KONST);
            break;
        case PLUM_OP_FIELD_SUB_INT_K:
            FIELD_CHECKED(sub_overflows, "-", KONST);
            break;
        case PLUM_OP_FIELD_ADD_INT_L:
            FIELD_CHECKED(add_overflows, "+", SLOT);
            break;
        case PLUM_OP_FIELD_SUB_INT_L:
            FIELD_CHECKED(sub_overflows, "-", SLOT);
            break;
        case PLUM_OP_FIELD_ADD_FLOAT_K:
            FIELD_FLOAT(+, KONST);
            break;
        case PLUM_OP_FIELD_SUB_FLOAT_K:
            FIELD_FLOAT(-, KONST);
            break;
        case PLUM_OP_FIELD_ADD_FLOAT_L:
            FIELD_FLOAT(+, SLOT);
            break;
        case PLUM_OP_FIELD_SUB_FLOAT_L:
            FIELD_FLOAT(-, SLOT);
            break;
        case PLUM_OP_IS_TREE: {
            PlumTreeInfo *tree = plum_as_tree_info(chunk->constants.items[READ_U16()]);
            PlumValue v = pop(vm);
            bool is = plum_is_obj(v, PLUM_OBJ_OBJECT) &&
                      plum_tree_is(plum_as_object(v)->tree, tree);
            push(vm, plum_bool(is));
            plum_release(v);
            break;
        }
        case PLUM_OP_WEAK: {
            PlumValue *v = peek(vm, 0);
            if (plum_is_ref(*v)) {
                PlumWeak *w = plum_weak_of(plum_as_object(*v));
                plum_release(*v);
                *v = plum_obj(&w->obj);
            }
            break;
        }
        case PLUM_OP_UNWEAK: {
            PlumValue *v = peek(vm, 0);
            if (plum_is_ref(*v)) {
                PlumObject *target = ((PlumWeak *)plum_as_obj(*v))->target;
                PlumValue strong =
                    target != NULL ? plum_retain(plum_obj(&target->obj)) : plum_none();
                plum_release(*v);
                *v = strong;
            }
            break;
        }
        case PLUM_OP_SPAWN: {
            uint16_t argc = READ_U16();
            push(vm, plum_obj(&spawn(vm, argc)->obj));
            break;
        }
        case PLUM_OP_CALL_NATIVE: {
            const PlumNativeFunction *fn = plum_native_by_id(READ_U16());
            uint16_t argc = READ_U16();
            /* A copy of the arguments: the function may run Plumlang code
             * (to_str), which can move the stack. The stack keeps them alive. */
            PlumValue local[8];
            PlumValue *args = argc <= 8 ? local : plum_alloc_zeroed(argc, sizeof(PlumValue));
            if (argc > 0) {
                memcpy(args, &vm->stack[vm->top - argc], argc * sizeof(PlumValue));
            }
            PlumValue result = plum_none();
            SAVE_IP();
            bool ok = vm->options->sandbox && outside_world(fn, vm->options)
                          ? plum_native_throw((PlumNative *)(void *)vm, PLUM_ERROR_IO,
                                              "%s() isn't allowed in this run (sandbox)", fn->name)
                          : fn->fn((PlumNative *)(void *)vm, args, argc, &result);
            if (args != local) {
                plum_free(args);
            }
            if (!ok && vm->yielded) {
                /* the task waits: this call runs again when it is resumed */
                frame->ip = op_start;
                return false;
            }
            end_native_call(vm);
            if (!ok) {
                goto thrown;
            }
            unwind_to(vm, vm->top - argc);
            push(vm, result);
            break;
        }
        case PLUM_OP_IS_TRAIT: {
            const PlumString *trait = plum_as_string(chunk->constants.items[READ_U16()]);
            PlumValue v = pop(vm);
            bool is = false;
            if (plum_is_obj(v, PLUM_OBJ_OBJECT)) {
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
            Frame *callee = invoke(vm, fn, argc);
            if (callee == NULL) {
                goto thrown;
            }
            frame = callee;
            ip = frame->ip;
            chunk = fn->chunk;
            break;
        }
        case PLUM_OP_CALL_METHOD: {
            uint16_t m = READ_U16();
            uint16_t argc = READ_U16();
            SAVE_IP();
            if (m >= vm->method_count) {
                FAIL(PLUM_ERROR_ERROR, "internal error: no method function %d", (int)m);
            }
            PlumFunction *fn = vm->methods[m];
            Frame *callee = invoke(vm, fn, argc);
            if (callee == NULL) {
                goto thrown;
            }
            frame = callee;
            ip = frame->ip;
            chunk = fn->chunk;
            break;
        }
        case PLUM_OP_IS_NONE: {
            PlumValue v = pop(vm);
            push(vm, plum_bool(plum_is_none(v)));
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
            if (!plum_as_bool(pop(vm))) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_FALSE_KEEP: {
            uint16_t d = READ_U16();
            if (!plum_as_bool(*peek(vm, 0))) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_TRUE_KEEP: {
            uint16_t d = READ_U16();
            if (plum_as_bool(*peek(vm, 0))) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_NOT_NONE_KEEP: {
            uint16_t d = READ_U16();
            if (!plum_is_none(*peek(vm, 0))) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_NONE_KEEP: {
            uint16_t d = READ_U16();
            if (plum_is_none(*peek(vm, 0))) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_JUMP_IF_NOT_LT_INT:
            JUMP_UNLESS_INT(<);
            break;
        case PLUM_OP_JUMP_IF_NOT_LE_INT:
            JUMP_UNLESS_INT(<=);
            break;
        case PLUM_OP_JUMP_IF_NOT_GT_INT:
            JUMP_UNLESS_INT(>);
            break;
        case PLUM_OP_JUMP_IF_NOT_GE_INT:
            JUMP_UNLESS_INT(>=);
            break;
        case PLUM_OP_JUMP_IF_NOT_EQ_INT:
            JUMP_UNLESS_INT(==);
            break;
        case PLUM_OP_JUMP_IF_NOT_NE_INT:
            JUMP_UNLESS_INT(!=);
            break;
        case PLUM_OP_JUMP_IF_NOT_LT_FLOAT:
            JUMP_UNLESS_FLOAT(<);
            break;
        case PLUM_OP_JUMP_IF_NOT_LE_FLOAT:
            JUMP_UNLESS_FLOAT(<=);
            break;
        case PLUM_OP_JUMP_IF_NOT_GT_FLOAT:
            JUMP_UNLESS_FLOAT(>);
            break;
        case PLUM_OP_JUMP_IF_NOT_GE_FLOAT:
            JUMP_UNLESS_FLOAT(>=);
            break;
        case PLUM_OP_JUMP_IF_NOT_EQ_FLOAT:
            JUMP_UNLESS_FLOAT(==);
            break;
        case PLUM_OP_JUMP_IF_NOT_NE_FLOAT:
            JUMP_UNLESS_FLOAT(!=);
            break;
        case PLUM_OP_JUMP_IF_NOT_LT_INT_K:
            JUMP_UNLESS_INT_K(<);
            break;
        case PLUM_OP_JUMP_IF_NOT_LE_INT_K:
            JUMP_UNLESS_INT_K(<=);
            break;
        case PLUM_OP_JUMP_IF_NOT_GT_INT_K:
            JUMP_UNLESS_INT_K(>);
            break;
        case PLUM_OP_JUMP_IF_NOT_GE_INT_K:
            JUMP_UNLESS_INT_K(>=);
            break;
        case PLUM_OP_JUMP_IF_NOT_EQ_INT_K:
            JUMP_UNLESS_INT_K(==);
            break;
        case PLUM_OP_JUMP_IF_NOT_NE_INT_K:
            JUMP_UNLESS_INT_K(!=);
            break;
        case PLUM_OP_JUMP_IF_NOT_LT_INT_LK:
            JUMP_UNLESS_LK(<);
            break;
        case PLUM_OP_JUMP_IF_NOT_LE_INT_LK:
            JUMP_UNLESS_LK(<=);
            break;
        case PLUM_OP_JUMP_IF_NOT_GT_INT_LK:
            JUMP_UNLESS_LK(>);
            break;
        case PLUM_OP_JUMP_IF_NOT_GE_INT_LK:
            JUMP_UNLESS_LK(>=);
            break;
        case PLUM_OP_JUMP_IF_NOT_EQ_INT_LK:
            JUMP_UNLESS_LK(==);
            break;
        case PLUM_OP_JUMP_IF_NOT_NE_INT_LK:
            JUMP_UNLESS_LK(!=);
            break;
        case PLUM_OP_JUMP_IF_NOT_LT_FLOAT_K:
            JUMP_UNLESS_FLOAT_K(<);
            break;
        case PLUM_OP_JUMP_IF_NOT_LE_FLOAT_K:
            JUMP_UNLESS_FLOAT_K(<=);
            break;
        case PLUM_OP_JUMP_IF_NOT_GT_FLOAT_K:
            JUMP_UNLESS_FLOAT_K(>);
            break;
        case PLUM_OP_JUMP_IF_NOT_GE_FLOAT_K:
            JUMP_UNLESS_FLOAT_K(>=);
            break;
        case PLUM_OP_JUMP_IF_NOT_EQ_FLOAT_K:
            JUMP_UNLESS_FLOAT_K(==);
            break;
        case PLUM_OP_JUMP_IF_NOT_NE_FLOAT_K:
            JUMP_UNLESS_FLOAT_K(!=);
            break;
        case PLUM_OP_LOOP: {
            uint16_t d = READ_U16();
            ip -= d;
            if (!step(vm)) {
                SAVE_IP();
                goto thrown;
            }
            break;
        }
        case PLUM_OP_FOR_RANGE: {
            uint16_t slot = READ_U16();
            uint16_t inclusive = READ_U16();
            uint16_t d = READ_U16();
            int64_t i = plum_as_int(SLOT(slot)), end = plum_as_int(SLOT(slot + 1));
            if (inclusive ? i > end : i >= end) {
                ip += d;
            }
            break;
        }
        case PLUM_OP_FOR_NEXT: {
            uint16_t slot = READ_U16();
            uint16_t inclusive = READ_U16();
            uint16_t d = READ_U16();
            /* Go round again if the next value is still in the range: then
             * adding 1 can't overflow, even at an inclusive end that is the
             * largest int. The jump goes back past FOR_RANGE, to the body. */
            PlumValue *v = &SLOT(slot);
            int64_t i, end;
            bool small = plum_both_small_ints(v[0], v[1]);
            if (small) {
                i = plum_small_int_of(v[0]);
                end = plum_small_int_of(v[1]);
            } else {
                i = plum_as_int(v[0]);
                end = plum_as_int(v[1]);
            }
            if (i < end && (inclusive || i + 1 < end)) {
                if (small) {
                    set_small(v, i + 1); /* below `end`: inline too */
                } else {
                    plum_set_int(v, i + 1);
                }
                ip -= d;
                if (!step(vm)) {
                    SAVE_IP();
                    goto thrown;
                }
            }
            break;
        }

        case PLUM_OP_CALL: {
            uint16_t argc = READ_U16();
            PlumValue callee = *peek(vm, argc);
            PlumFunction *fn = plum_as_obj(callee)->type == PLUM_OBJ_CLOSURE ? plum_as_closure(callee)->fn
                                                                       : plum_as_function(callee);
            SAVE_IP();
            Frame *f = call_frame(vm, fn, argc, 1);
            if (f == NULL) {
                goto thrown;
            }
            frame = f;
            ip = f->ip;
            chunk = fn->chunk;
            break;
        }
        case PLUM_OP_CALL_GLOBAL: {
            /* A function the program declared, called by name with every
             * argument (#120): the function stays in its global, not
             * pushed, and the arguments become its slots where they are. */
            uint16_t g = READ_U16();
            uint16_t argc = READ_U16();
            CHECK_GLOBAL(g);
            PlumFunction *fn = plum_as_function(vm->globals[g]);
            SAVE_IP();
            Frame *f = call_frame(vm, fn, argc, 0);
            if (f == NULL) {
                goto thrown;
            }
            frame = f;
            ip = f->ip;
            chunk = fn->chunk;
            break;
        }
        case PLUM_OP_CALL_SELF: {
            /* A declared function calling itself by name. */
            uint16_t argc = READ_U16();
            PlumFunction *fn = frame->fn;
            SAVE_IP();
            Frame *f = call_frame(vm, fn, argc, 0);
            if (f == NULL) {
                goto thrown;
            }
            frame = f;
            ip = f->ip;
            break;
        }
        case PLUM_OP_TAIL_SELF: {
            /* return f(...) in f, outside any try: the arguments replace
             * the running call's slots and it starts again (spec 6.5). It
             * counts as a step, as a call does. */
            uint16_t argc = READ_U16();
            if (!step(vm)) {
                SAVE_IP();
                goto thrown;
            }
            PlumValue *slots = &vm->stack[frame->base];
            PlumValue *args = &vm->stack[vm->top - argc];
            release_values(slots, args); /* the old slots, and anything above them */
            memmove(slots, args, (size_t)argc * sizeof(PlumValue));
            vm->top = frame->base + (size_t)argc;
            for (int i = argc; i < frame->fn->slot_count; i++) {
                vm->stack[vm->top++] = plum_none();
            }
            if (frame->tail_calls < 0x7FFFFFFF) {
                frame->tail_calls++;
            }
            ip = chunk->code.items;
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
        case PLUM_OP_RETURN_LOCAL: {
            const PlumValue *v = &SLOT(READ_U16());
            copy_value(&ret, v);
            if (plum_is_ref(ret)) {
                plum_as_obj(ret)->refcount++;
            }
            goto do_return;
        }
        case PLUM_OP_RETURN_NONE:
            ret = plum_none();
            goto do_return;
        case PLUM_OP_RETURN_ADD_INT: {
            PlumValue *s = &vm->stack[vm->top - 2];
            int64_t r;
            if (INT_FAST(add_overflows, s[0], s[1], r)) {
                push_int(&ret, r);
            } else {
                SAVE_IP();
                if (!int_arith_slow(vm, '+', &s[0], s[1], 0)) {
                    goto thrown; /* both are still on the stack */
                }
                plum_release_int(s[1]);
                ret = s[0];
            }
            vm->top -= 2;
            goto do_return;
        }
        case PLUM_OP_RETURN: {
            copy_value(&ret, &vm->stack[--vm->top]);
        do_return:;
            int n = --vm->frame_count;
            while (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame_count > n) {
                vm->handler_count--; /* tries the function returned out of */
            }
            /* the slots, what is left above them, and the function if pushed */
            size_t to = frame->base - frame->callee;
            release_values(&vm->stack[to], &vm->stack[vm->top]);
            copy_value(&vm->stack[to], &ret);
            vm->top = to + 1;
            if (n == vm->stop_at) {
                return true; /* back to whoever started this run */
            }
            /* the caller's frame: the one before, unless a segment starts here */
            frame = (unsigned)n % FRAME_SEG != 0 ? frame - 1 : FRAME(vm, n - 1);
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
        ENTER_FRAME();
    }

#undef READ_U16
#undef SLOT
#undef SAVE_IP
#undef FAIL
#undef INT_BINARY
#undef FLOAT_BINARY
#undef CHECKED
#undef STR_COMPARE
#undef KONST
#undef CHECKED_WITH
#undef FLOAT_WITH
#undef JUMP_UNLESS_INT
#undef JUMP_UNLESS_FLOAT
#undef JUMP_UNLESS_INT_K
#undef JUMP_UNLESS_FLOAT_K
#undef OVERFLOW
#undef INT_FAST
#undef DROP_X
#undef DROP_Y
#undef JUMP_UNLESS_LK
#undef PLACE_CHECKED
#undef NO_CHECK
#undef FIELD_OF
#undef FIELD_CHECKED
#undef FIELD_FLOAT
#undef GLOBAL
#undef LOCAL_CHECKED
#undef GLOBAL_CHECKED
#undef PUSH_INT
#undef PUSH_CHECKED
#undef ENTER_FRAME
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

/* Each kind of value made has a counter in PlumAllocStats. */
typedef char plum_made_kinds_fit[PLUM_MADE_COUNT <= PLUM_ALLOC_COUNTERS ? 1 : -1];

/* n with a comma every three digits: 1,234,567. */
static const char *grouped(uint64_t n, char buf[32]) {
    char digits[24];
    int len = snprintf(digits, sizeof digits, "%llu", (unsigned long long)n);
    int at = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            buf[at++] = ',';
        }
        buf[at++] = digits[i];
    }
    buf[at] = '\0';
    return buf;
}

/* A number of bytes, and in KB or MB when it is that big. */
static void print_bytes(FILE *err, const char *what, int64_t bytes) {
    char buf[32];
    uint64_t n = bytes > 0 ? (uint64_t)bytes : 0;
    fprintf(err, "  %-20s %s bytes", what, grouped(n, buf));
    if (n >= 1024 * 1024) {
        fprintf(err, " (%.1f MB)", (double)n / (1024.0 * 1024.0));
    } else if (n >= 1024) {
        fprintf(err, " (%.1f KB)", (double)n / 1024.0);
    }
    fputc('\n', err);
}

/* plum run --mem-stats: what the run used, counted from its start to its end. */
static void print_mem_stats(FILE *err, const PlumAllocStats *s) {
    static const char *const kinds[PLUM_MADE_COUNT] = {
        "strings",         "lists",       "maps",     "sets",
        "tree objects",    "kind values", "closures", "captured variables",
        "weak references", "tasks",       "channels",
    };
    char buf[32];
    fprintf(err, "memory used by this run (--mem-stats):\n");
    if (s->bytes_known) {
        print_bytes(err, "peak in use", s->peak_bytes);
        print_bytes(err, "allocated in all", (int64_t)s->bytes_allocated);
        print_bytes(err, "still in use at exit", s->bytes_in_use);
    } else {
        fprintf(err, "  (this platform can't tell the size of a block, so only counts)\n");
    }
    fprintf(err, "  %-20s %s\n", "allocations", grouped(s->allocations, buf));
    fprintf(err, "  %-20s %s\n", "resizes", grouped(s->resizes, buf));
    fprintf(err, "  %-20s %s\n", "frees", grouped(s->frees, buf));
    fprintf(err, "values made:\n");
    for (int i = 0; i < PLUM_MADE_COUNT; i++) {
        fprintf(err, "  %-20s %s\n", kinds[i], grouped(s->counts[i], buf));
    }
}

static VM *vm_create(const PlumRunOptions *options, FILE *out, FILE *err) {
    VM *vm = plum_alloc_zeroed(1, sizeof *vm);
    vm->out = out;
    vm->err = err;
    plum_random_key(vm->seed);
    vm->globals = plum_alloc_zeroed(1, sizeof(PlumValue));
    vm->options = options;
    vm->limited = options->max_steps > 0 || options->max_memory > 0;
    uint64_t key[2];
    plum_random_key(key);
    plum_native_seed_random((PlumNative *)(void *)vm, key[0] ^ key[1]);
    vm->text_hook = (PlumTextHook){object_text, vm, &vm->text_depth};
    vm->thrown = plum_none();
    vm->trace_of = plum_none();
    return vm;
}

/* Take in what a compiled program (or REPL entry) needs: room for its
 * globals (new ones start unset), its error trees and method numbers. */
static void vm_load(VM *vm, const PlumCompiled *program, const PlumSource *source) {
    vm->source = source;
    if (program->global_count > vm->global_count) {
        vm->globals =
            plum_realloc_array(vm->globals, (size_t)program->global_count, sizeof(PlumValue));
        for (int i = vm->global_count; i < program->global_count; i++) {
            vm->globals[i] = unset();
        }
        vm->global_count = program->global_count;
    }
    vm->errors = program->errors;
    vm->to_str = program->to_str;
    vm->equals = program->equals;
    vm->compare = program->compare;
    if (program->methods != NULL) {
        vm->methods = program->methods;
        vm->method_count = program->method_count;
    }
}

/* The REPL: an entry's main task that couldn't finish (a deadlock) is
 * stopped. Channels and other tasks may still point at it, so it stays
 * allocated (parked) until the session ends; it never runs again. */
static void park_task(VM *vm, PlumTask *t) {
    for (size_t i = 0; i < t->top; i++) {
        plum_release(t->stack[i]);
    }
    t->top = 0;
    if (t->call_state != NULL && t->call_state_free != NULL) {
        t->call_state_free(t->call_state);
    }
    t->call_state = NULL;
    t->call_state_free = NULL;
    t->waiters.len = 0;
    t->queued = false;
    t->state = TASK_FAILED; /* so nothing wakes it */
    t->waited = true;
    plum_array_push(&vm->parked, t); /* the reference moves */
}

/* What's still running when the program (or REPL session) ends is dropped. */
static void drop_tasks(VM *vm) {
    plum_poller_free(vm->poller);
    vm->poller = NULL;
    for (size_t i = 0; i < vm->timers.len; i++) {
        plum_release(plum_obj(&vm->timers.items[i].task->obj));
    }
    plum_array_free(&vm->timers);
    for (size_t i = 0; i < vm->tasks.len; i++) {
        plum_release(plum_obj(&vm->tasks.items[i]->obj));
    }
    plum_array_free(&vm->tasks);
    plum_array_free(&vm->ready);
    for (size_t i = 0; i < vm->parked.len; i++) {
        plum_release(plum_obj(&vm->parked.items[i]->obj));
    }
    plum_array_free(&vm->parked);
}

/* Run `main_fn` as the main task, until it ends; report what went wrong.
 * Returns 0, 1 after an error, or the code given to exit(). */
static int vm_run_main(VM *vm, PlumFunction *main_fn) {
    FILE *out = vm->out;
    FILE *err = vm->err;
    vm->exit_code = 0;
    /* The program's own code is the main task; <main> sits at the bottom
     * of its stack, like any called function. */
    push(vm, plum_retain(plum_obj(&main_fn->obj)));
    if (vm->poller == NULL) {
        vm->poller = plum_poller_new();
    }
    vm->main_task = spawn(vm, 0);
    PlumTask *main = vm->main_task;
    plum_free(vm->stack); /* it only carried <main> into the first task */
    vm->stack = NULL;
    vm->top = 0;
    vm->capacity = 0;
    if (!run_tasks(vm, main_fn)) {
        fflush(out);
        fprintf(err,
                "%s: Error: deadlock: every task is waiting (for a channel or another task) "
                "and nothing is left to wake them\n",
                vm->source->path);
        vm->exit_code = 1;
    } else if (!vm->stopped && main->state == TASK_FAILED) {
        fflush(out);
        if (main->trace != NULL) {
            fputs(main->trace, err);
        }
        vm->exit_code = 1;
    }
    fflush(out);
    /* A task that failed must not fail silently. */
    for (size_t i = 0; i < vm->failed.len && !vm->stopped; i++) {
        PlumTask *t = vm->failed.items[i];
        if (!t->waited) {
            fprintf(err, "error in a task that nothing waited for:\n");
            if (t->trace != NULL) {
                fputs(t->trace, err);
            }
            if (vm->exit_code == 0) {
                vm->exit_code = 1;
            }
        }
    }
    for (size_t i = 0; i < vm->failed.len; i++) {
        plum_release(plum_obj(&vm->failed.items[i]->obj));
    }
    plum_array_free(&vm->failed);
    if (!vm->session || vm->stopped) {
        drop_tasks(vm);
    } else {
        /* The REPL: the other tasks carry on in the next entry; the main
         * task leaves (it is still in the task list when it ends). */
        PlumTask *last = vm->tasks.items[--vm->tasks.len];
        vm->tasks.items[main->index] = last;
        last->index = main->index;
        if (task_ended(main)) {
            plum_release(plum_obj(&main->obj));
        } else {
            park_task(vm, main);
        }
    }
    plum_release(plum_obj(&main->obj));
    vm->main_task = NULL;
    vm->current = NULL;
    vm->yielded = false;
    unwind_to(vm, 0);
    return vm->exit_code;
}

/* Release the globals and free the VM; with --check-leaks, report what
 * cycles kept alive. Returns `code`, or 1 if the leak check failed. */
static int vm_destroy(VM *vm, int code) {
    drop_tasks(vm);
    for (int i = 0; i < vm->global_count; i++) {
        plum_release(vm->globals[i]);
    }
    plum_release(vm->thrown);
    plum_release(vm->trace_of);
    if (vm->options->check_leaks) {
        fflush(vm->out);
        if (check_leaks(vm) > 0 && code == 0) {
            code = 1;
        }
    }
    for (size_t i = 0; i < vm->tracked.len; i++) {
        plum_release(plum_obj(&vm->tracked.items[i]->obj));
    }
    plum_array_free(&vm->tracked);
    for (size_t i = 0; i < vm->data.len; i++) {
        ProgramData *d = &vm->data.items[i];
        if (d->value != NULL && d->free_fn != NULL) {
            d->free_fn(d->value);
        }
    }
    plum_array_free(&vm->data);
    plum_free(vm->globals);
    plum_free(vm->trace);
    plum_free(vm->handlers);
    for (int i = 0; i < FRAME_SEGS; i++) {
        plum_free(vm->segs[i]);
    }
    plum_free(vm->stack);
    plum_free(vm);
    return code;
}

int plum_vm_run(const PlumCompiled *program, const PlumSource *source,
                const PlumRunOptions *options, FILE *out, FILE *err) {
    /* --mem-stats: the run's own counters, which the allocator counts into
     * until the run is over (util/alloc.h). */
    PlumAllocStats stats;
    bool counting = options->mem_stats || options->max_memory > 0;
    if (counting) {
        plum_alloc_set_stats(&stats);
    }
    VM *vm = vm_create(options, out, err);
    vm->stats = counting ? &stats : NULL;
    vm_load(vm, program, source);
    int code = vm_run_main(vm, program->main);
    code = vm_destroy(vm, code);
    if (counting) {
        plum_alloc_set_stats(NULL);
    }
    if (options->mem_stats) {
        fflush(out);
        print_mem_stats(err, &stats);
    }
    return code;
}

PlumVM *plum_vm_new(const PlumRunOptions *options, FILE *out, FILE *err) {
    VM *vm = vm_create(options, out, err);
    vm->session = true;
    return vm;
}

int plum_vm_run_entry(PlumVM *vm, const PlumCompiled *program, const PlumSource *source) {
    vm_load(vm, program, source);
    /* What the last entry threw is gone; this one starts clean. */
    plum_release(vm->thrown);
    vm->thrown = plum_none();
    plum_release(vm->trace_of);
    vm->trace_of = plum_none();
    vm->trace_len = 0;
    if (vm->trace != NULL) {
        vm->trace[0] = '\0';
    }
    return vm_run_main(vm, program->main);
}

bool plum_vm_exited(const PlumVM *vm) {
    return vm->stopped;
}

void plum_vm_free(PlumVM *vm) {
    if (vm != NULL) {
        vm_destroy(vm, 0);
    }
}
