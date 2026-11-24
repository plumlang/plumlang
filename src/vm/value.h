/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Runtime values (spec section 11). A value is a small tagged union: none,
 * a bool, an int, a float, or a reference to a heap object. Objects are
 * reference counted: every place that holds an object value (a stack slot,
 * a variable, a constant pool) owns one reference, and the object is freed
 * the moment the last one is released. There is no garbage collector.
 */
#ifndef PLUM_VM_VALUE_H
#define PLUM_VM_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/alloc.h"

typedef enum PlumValueType {
    PLUM_VAL_NONE,
    PLUM_VAL_BOOL,
    PLUM_VAL_INT, /* int and byte */
    PLUM_VAL_FLOAT,
    PLUM_VAL_OBJ,
} PlumValueType;

typedef enum PlumObjType {
    PLUM_OBJ_STRING,
    PLUM_OBJ_FUNCTION,
    PLUM_OBJ_LIST,
    PLUM_OBJ_MAP,       /* maps and sets: vm/table.h */
    PLUM_OBJ_CELL,      /* a captured variable's box */
    PLUM_OBJ_CLOSURE,   /* a function with the cells it captured */
    PLUM_OBJ_TREE_INFO, /* what the VM knows about a tree: name, parent, fields */
    PLUM_OBJ_OBJECT,    /* an object of a tree */
    PLUM_OBJ_WEAK,      /* a weak reference to an object */
    PLUM_OBJ_TASK,      /* a task (spawn): defined in vm.c, which owns its running state */
    PLUM_OBJ_CHANNEL,   /* a channel between tasks: defined in vm.c */
    PLUM_OBJ_HANDLE,    /* a resource of the operating system: a socket */
    PLUM_OBJ_INT,       /* an int too large to store inline (PLUM_NANBOX) */
} PlumObjType;

/*
 * What `plum run --mem-stats` counts as a run makes values: the counters
 * of PlumAllocStats (util/alloc.h), through plum_alloc_count.
 */
typedef enum PlumMadeKind {
    PLUM_MADE_STRING,
    PLUM_MADE_LIST,
    PLUM_MADE_MAP,
    PLUM_MADE_SET,
    PLUM_MADE_OBJECT, /* of a tree */
    PLUM_MADE_KIND,   /* a kind value with contents */
    PLUM_MADE_CLOSURE,
    PLUM_MADE_CELL, /* a variable a closure captured */
    PLUM_MADE_WEAK,
    PLUM_MADE_TASK,
    PLUM_MADE_CHANNEL,
    PLUM_MADE_COUNT
} PlumMadeKind;

/*
 * The header every heap object starts with. The count is a naturally
 * aligned word of its own, with no other bits packed into it, so that it
 * can one day be changed atomically for a frozen object
 * (docs/design/concurrency.md).
 */
typedef struct PlumObj {
    uint32_t refcount;
    uint8_t type;  /* PlumObjType */
    uint8_t flags; /* PLUM_OBJ_FROZEN */
} PlumObj;

/*
 * A frozen object is never changed in place, whatever its count reads:
 * copy-on-write always copies it. The compiler freezes its constant
 * strings.
 */
#define PLUM_OBJ_FROZEN 0x01u

/* Is the holder of this one reference its only owner, free to change it in place? */
static inline bool plum_obj_unique(const PlumObj *o) {
    return o->refcount == 1 && (o->flags & PLUM_OBJ_FROZEN) == 0;
}

/*
 * How a value is laid out is chosen when Plumlang is built (#121):
 *
 * PLUM_NANBOX=0, the wide layout: a type tag and an 8-byte payload, 16
 *   bytes in all (12 where int64_t is 4-byte aligned).
 *
 * PLUM_NANBOX=1, NaN boxing: 8 bytes. A float is its own IEEE 754 bits;
 *   everything else hides in the quiet NaNs with the sign bit set, which no
 *   float the VM makes ever has (a NaN it makes or takes in is one of the
 *   two with no payload, plum_float):
 *
 *     below 0xFFF9 0000 0000 0000   a float (-NaN is 0xFFF8 0000 0000 0000)
 *     0xFFF9 0000 0000 000x         none (x = 0), unset (1), bad (2), good (3)
 *     0xFFFA pppp pppp pppp         a heap object at the 48-bit address p
 *     0xFFFC ... to 0xFFFF ...      an int from -2^49 to 2^49 - 1, stored as
 *                                   i + 0xFFFE 0000 0000 0000 (mod 2^64)
 *
 *   An int outside that range is boxed: a PlumBigInt on the heap, held
 *   like any object, so an int is still a full 64-bit int with the same
 *   overflow errors. An int that fits inline is never boxed, so two inline
 *   ints are equal exactly when their bits are, and their bits order them
 *   as unsigned numbers.
 *
 * Code outside this header reads and makes values only through the
 * functions below, so it compiles to either layout.
 */
#ifndef PLUM_NANBOX
#define PLUM_NANBOX 0
#endif

/* An int too large to store inline (NaN boxing only): immutable. */
typedef struct PlumBigInt {
    PlumObj obj;
    int64_t i;
} PlumBigInt;

#if PLUM_NANBOX

typedef struct PlumValue {
    uint64_t bits;
} PlumValue;

#define PLUM_NB_SPECIAL   UINT64_C(0xFFF9000000000000) /* below it: a float */
#define PLUM_NB_OBJ       UINT64_C(0xFFFA000000000000)
#define PLUM_NB_INT       UINT64_C(0xFFFC000000000000) /* at or above it: an inline int */
#define PLUM_NB_INT_ZERO  UINT64_C(0xFFFE000000000000) /* the int 0 */
#define PLUM_NB_ADDRESS   UINT64_C(0x0000FFFFFFFFFFFF)
#define PLUM_NB_QUIET_NAN UINT64_C(0x7FF8000000000000)
#define PLUM_NB_SIGN      UINT64_C(0x8000000000000000)

/* The smallest and largest int stored inline. */
#define PLUM_INLINE_INT_MIN (-(INT64_C(1) << 49))
#define PLUM_INLINE_INT_MAX ((INT64_C(1) << 49) - 1)

static inline PlumValue plum_value_of_bits(uint64_t bits) {
    PlumValue v;
    v.bits = bits;
    return v;
}

static inline PlumValue plum_none(void) {
    return plum_value_of_bits(PLUM_NB_SPECIAL);
}

static inline PlumValue plum_bool(bool b) {
    return plum_value_of_bits(PLUM_NB_SPECIAL | 2u | (b ? 1u : 0u));
}

/* Does `i` fit inline? */
static inline bool plum_int_inline(int64_t i) {
    return (uint64_t)i + (UINT64_C(1) << 49) < (UINT64_C(1) << 50);
}

/* The inline int `i`, which must fit (plum_int_inline). */
static inline PlumValue plum_small_int(int64_t i) {
    return plum_value_of_bits((uint64_t)i + PLUM_NB_INT_ZERO);
}

/* Is `v` an int stored inline? */
static inline bool plum_is_small_int(PlumValue v) {
    return v.bits >= PLUM_NB_INT;
}

/* Are both inline ints? */
static inline bool plum_both_small_ints(PlumValue a, PlumValue b) {
    return (a.bits & b.bits) >= PLUM_NB_INT;
}

/* The int stored inline in `v`. */
static inline int64_t plum_small_int_of(PlumValue v) {
    return (int64_t)(v.bits - PLUM_NB_INT_ZERO);
}

/* An int outside the inline range, boxed (value.c). */
PlumValue plum_int_boxed(int64_t i);

static inline PlumValue plum_int(int64_t i) {
    return plum_int_inline(i) ? plum_small_int(i) : plum_int_boxed(i);
}

/*
 * A float, as it is stored. A NaN becomes the one of its sign with no
 * payload, so no NaN (whatever strtod or the hardware made) lands among the
 * other values. Arithmetic on these two gives one of them again (IEEE 754,
 * and WebAssembly's rule for canonical NaNs), so the VM's float
 * instructions store their results with plum_float_raw.
 */
static inline PlumValue plum_float_raw(double f) {
    PlumValue v;
    memcpy(&v.bits, &f, sizeof f);
    return v;
}

static inline PlumValue plum_float(double f) {
    PlumValue v = plum_float_raw(f);
    if (f != f) {
        v.bits = (v.bits & PLUM_NB_SIGN) | PLUM_NB_QUIET_NAN;
    }
    return v;
}

static inline double plum_as_float(PlumValue v) {
    double f;
    memcpy(&f, &v.bits, sizeof f);
    return f;
}

static inline PlumValue plum_obj(PlumObj *obj) {
#if UINTPTR_MAX > UINT32_MAX
    if (((uint64_t)(uintptr_t)obj >> 48) != 0) {
        plum_fatal("an object's address doesn't fit in 48 bits; build with PLUM_NANBOX=0");
    }
#endif
    return plum_value_of_bits(PLUM_NB_OBJ | (uint64_t)(uintptr_t)obj);
}

/* Does `v` hold a reference (an object, or a boxed int)? */
static inline bool plum_is_ref(PlumValue v) {
    return (v.bits >> 48) == (PLUM_NB_OBJ >> 48);
}

/* The heap object a reference points at. */
static inline PlumObj *plum_as_obj(PlumValue v) {
    return (PlumObj *)(uintptr_t)(v.bits & PLUM_NB_ADDRESS);
}

static inline bool plum_is_none(PlumValue v) {
    return (v.bits & ~UINT64_C(1)) == PLUM_NB_SPECIAL; /* none or unset */
}

static inline bool plum_as_bool(PlumValue v) {
    return (v.bits & 1u) != 0;
}

static inline int64_t plum_as_int(PlumValue v) {
    return plum_is_small_int(v) ? plum_small_int_of(v) : ((const PlumBigInt *)plum_as_obj(v))->i;
}

static inline PlumValueType plum_value_type(PlumValue v) {
    if (v.bits < PLUM_NB_SPECIAL) {
        return PLUM_VAL_FLOAT;
    }
    if (v.bits >= PLUM_NB_INT) {
        return PLUM_VAL_INT;
    }
    if (plum_is_ref(v)) {
        return plum_as_obj(v)->type == PLUM_OBJ_INT ? PLUM_VAL_INT : PLUM_VAL_OBJ;
    }
    return (v.bits & 2u) != 0 ? PLUM_VAL_BOOL : PLUM_VAL_NONE;
}

/* Is the whole of `v` the same as the whole of `w` (not equality of
 * values: two boxed ints can be equal and differ)? */
static inline bool plum_same_bits(PlumValue v, PlumValue w) {
    return v.bits == w.bits;
}

#else /* the wide layout */

typedef struct PlumValue {
    PlumValueType type;
    union {
        bool b;
        int64_t i;
        double f;
        PlumObj *obj;
    } as;
} PlumValue;

static inline PlumValue plum_none(void) {
    PlumValue v;
    v.type = PLUM_VAL_NONE;
    v.as.i = 0;
    return v;
}

static inline PlumValue plum_bool(bool b) {
    PlumValue v;
    v.type = PLUM_VAL_BOOL;
    v.as.i = 0;
    v.as.b = b;
    return v;
}

static inline bool plum_int_inline(int64_t i) {
    (void)i;
    return true;
}

static inline PlumValue plum_int(int64_t i) {
    PlumValue v;
    v.type = PLUM_VAL_INT;
    v.as.i = i;
    return v;
}

static inline PlumValue plum_small_int(int64_t i) {
    return plum_int(i);
}

static inline bool plum_is_small_int(PlumValue v) {
    (void)v;
    return true;
}

static inline bool plum_both_small_ints(PlumValue a, PlumValue b) {
    (void)a;
    (void)b;
    return true;
}

static inline int64_t plum_small_int_of(PlumValue v) {
    return v.as.i;
}

static inline PlumValue plum_float(double f) {
    PlumValue v;
    v.type = PLUM_VAL_FLOAT;
    v.as.f = f;
    return v;
}

static inline PlumValue plum_float_raw(double f) {
    return plum_float(f);
}

static inline double plum_as_float(PlumValue v) {
    return v.as.f;
}

static inline PlumValue plum_obj(PlumObj *obj) {
    PlumValue v;
    v.type = PLUM_VAL_OBJ;
    v.as.obj = obj;
    return v;
}

static inline bool plum_is_ref(PlumValue v) {
    return v.type == PLUM_VAL_OBJ;
}

static inline PlumObj *plum_as_obj(PlumValue v) {
    return v.as.obj;
}

static inline bool plum_is_none(PlumValue v) {
    return v.type == PLUM_VAL_NONE;
}

static inline bool plum_as_bool(PlumValue v) {
    return v.as.b;
}

static inline int64_t plum_as_int(PlumValue v) {
    return v.as.i;
}

static inline PlumValueType plum_value_type(PlumValue v) {
    return v.type;
}

static inline bool plum_same_bits(PlumValue v, PlumValue w) {
    return v.type == w.type && v.as.i == w.as.i;
}

#endif /* PLUM_NANBOX */

static inline bool plum_is_int(PlumValue v) {
    return plum_value_type(v) == PLUM_VAL_INT;
}

static inline bool plum_is_float(PlumValue v) {
    return plum_value_type(v) == PLUM_VAL_FLOAT;
}

static inline bool plum_is_bool(PlumValue v) {
    return plum_value_type(v) == PLUM_VAL_BOOL;
}

/* Is `v` a heap object (a string, list, ... but not a boxed int)? */
static inline bool plum_is_object(PlumValue v) {
    return plum_value_type(v) == PLUM_VAL_OBJ;
}

/*
 * Immutable UTF-8 text. `chars` points just past the struct, NUL-terminated,
 * with room for `cap` bytes before the NUL. A string only ever has more
 * room than its length when `+=` grew it in place, which it does only when
 * the string has one owner and isn't frozen (plum_string_append): to
 * everyone else a string never changes. Nothing caches a string's hash.
 */
typedef struct PlumString {
    PlumObj obj;
    size_t len;
    char *chars;
    size_t char_count; /* characters (code points); valid once `counted` */
    bool counted;
    uint32_t cap; /* bytes `chars` can hold when more than `len` (a str made by
                   * `+=` is at most PLUM_MAX_STRING, which fits) */
} PlumString;

/*
 * A list is a value (spec 11.1): assigning one shares it, and a change
 * through a shared list first copies it (plum_list_unique). `items` holds
 * one reference per element.
 */
typedef struct PlumList {
    PlumObj obj;
    size_t len;
    size_t cap;
    PlumValue *items;
} PlumList;

/*
 * A tree at run time. Its objects store every field in one array, the
 * parent's fields first, so a field's position is fixed at compile time.
 */
typedef struct PlumTreeInfo {
    PlumObj obj;
    const char *name;
    struct PlumTreeInfo *parent; /* holds a reference, or NULL */
    int field_count;             /* parents' fields included */
    const char **field_names;    /* owned array; names are interned */
    /* What each method number runs on its objects (NULL: no such method);
     * the functions belong to the program, not to this table. */
    struct PlumFunction **methods;
    int method_count;
    const char **traits; /* every trait its objects have, by interned name */
    int trait_count;
    /* A kind's case ("Shape.Circle") instead of a tree: its objects are
     * values, compared by case and contents. NULL for a tree. */
    const char *kind;
} PlumTreeInfo;

/* An object of a tree: shared, not copied (spec 8.3). */
typedef struct PlumObject {
    PlumObj obj;
    PlumTreeInfo *tree;    /* holds a reference */
    PlumValue *fields;     /* tree->field_count values, just past the struct */
    struct PlumWeak *weak; /* its weak box, if anything refers to it weakly */
} PlumObject;

/*
 * A weak reference (spec 11.3): a box pointing at an object without
 * keeping it alive. Freeing the object empties the box, so reading it
 * gives none. An object has at most one box, shared by all its weak
 * references.
 */
typedef struct PlumWeak {
    PlumObj obj;
    PlumObject *target; /* NULL once the object is gone */
    uint32_t line;      /* where the object was made (for the leak check) */
} PlumWeak;

/*
 * A handle on something the operating system keeps: a socket. It closes
 * when the program closes it, or when its last reference goes.
 */
typedef struct PlumHandle {
    PlumObj obj;
    const char *kind; /* "Listener", "Connection", "UdpSocket" */
    intptr_t fd;      /* -1 once closed */
    void (*close)(struct PlumHandle *h);
    char *in; /* bytes read but not yet given out (read_line) */
    size_t in_len;
    size_t in_cap;
    char peer[96]; /* the other end; for UDP, the last sender */
    int port;      /* the local port */
    void *tls;     /* TLS: a connection's session, a listener's certificate */
} PlumHandle;

static inline PlumHandle *plum_as_handle(PlumValue v) {
    return (PlumHandle *)(void *)plum_as_obj(v);
}

/* Freeing tasks and channels, whose insides only the VM knows. */
void plum_task_destroy(PlumObj *task);
void plum_channel_destroy(PlumObj *channel);

/* The weak box of `o`, made if needed, with a new reference. */
PlumWeak *plum_weak_of(PlumObject *o);

typedef struct PlumChunk PlumChunk;

/* Where a closure gets each captured variable when it is made. */
typedef struct PlumCapture {
    bool from_local; /* a cell in the making function's slot `index`, or */
    uint16_t index;  /* that function's own capture `index` */
} PlumCapture;

typedef struct PlumFunction {
    PlumObj obj;
    const char *name; /* for stack traces; "<main>" for the top level */
    const char *path; /* the file it is written in; NULL: the program's own file */
    int arity;
    int slot_count;        /* parameters + locals */
    PlumChunk *chunk;      /* owned */
    PlumCapture *captures; /* owned; what MAKE_CLOSURE captures */
    int capture_count;
} PlumFunction;

/* A captured variable lives in a cell, so the function that declared it
 * and every closure that captured it share it (spec 6.4). */
typedef struct PlumCell {
    PlumObj obj;
    PlumValue value;
} PlumCell;

typedef struct PlumClosure {
    PlumObj obj;
    PlumFunction *fn; /* holds a reference */
    int count;
    PlumCell **cells; /* each holds a reference */
} PlumClosure;

/* Is `v` an object of `type`? */
static inline bool plum_is_obj(PlumValue v, PlumObjType type) {
    return plum_is_ref(v) && plum_as_obj(v)->type == type;
}

static inline PlumString *plum_as_string(PlumValue v) {
    return (PlumString *)plum_as_obj(v);
}

static inline PlumFunction *plum_as_function(PlumValue v) {
    return (PlumFunction *)plum_as_obj(v);
}

static inline PlumCell *plum_as_cell(PlumValue v) {
    return (PlumCell *)plum_as_obj(v);
}

static inline PlumClosure *plum_as_closure(PlumValue v) {
    return (PlumClosure *)plum_as_obj(v);
}

/* A function or closure value, either of which can be called. */
static inline bool plum_is_callable(PlumValue v) {
    return plum_is_ref(v) && (plum_as_obj(v)->type == PLUM_OBJ_FUNCTION ||
                              plum_as_obj(v)->type == PLUM_OBJ_CLOSURE);
}

static inline PlumTreeInfo *plum_as_tree_info(PlumValue v) {
    return (PlumTreeInfo *)plum_as_obj(v);
}

static inline PlumObject *plum_as_object(PlumValue v) {
    return (PlumObject *)plum_as_obj(v);
}

static inline PlumList *plum_as_list(PlumValue v) {
    return (PlumList *)plum_as_obj(v);
}

/* Take another reference to an object value (no-op for the rest). */
static inline PlumValue plum_retain(PlumValue v) {
    if (plum_is_ref(v)) {
        plum_as_obj(v)->refcount++;
    }
    return v;
}

/* Give up a reference; frees the object when it was the last one. */
void plum_release(PlumValue v);

/* Let go of an int value: a boxed one holds a reference. */
static inline void plum_release_int(PlumValue v) {
#if PLUM_NANBOX
    if (plum_is_ref(v)) {
        plum_release(v);
    }
#else
    (void)v;
#endif
}

/* Store `i` in the variable at `slot`, which holds an int. The wide layout
 * writes only the number: the type is already there. */
#if PLUM_NANBOX
void plum_set_int_slow(PlumValue *slot, int64_t i);
#endif

static inline void plum_set_int(PlumValue *slot, int64_t i) {
#if PLUM_NANBOX
    if (plum_int_inline(i) && !plum_is_ref(*slot)) {
        *slot = plum_small_int(i);
    } else {
        plum_set_int_slow(slot, i); /* boxing, or letting go of a box */
    }
#else
    slot->as.i = i;
#endif
}

/* Store the float `f` (a result of float arithmetic) in the variable at
 * `slot`, which holds a float. */
static inline void plum_set_float(PlumValue *slot, double f) {
#if PLUM_NANBOX
    *slot = plum_float_raw(f);
#else
    slot->as.f = f;
#endif
}

/*
 * The longest a string may be, in bytes, and a list, in items (spec 11.4).
 * Making a longer one is a ValueError, before any memory is asked for; no
 * realistic program comes near either. The fuzz targets build with
 * smaller ones (-D), to stay within the fuzzer's memory.
 */
#ifndef PLUM_MAX_STRING
#define PLUM_MAX_STRING ((size_t)1 << 30) /* 1 GiB */
#endif
#ifndef PLUM_MAX_LIST
#define PLUM_MAX_LIST ((size_t)1 << 27) /* 134,217,728 items */
#endif

/* A new string holding a copy of s[0..len), with one reference. */
PlumString *plum_string_new(const char *s, size_t len);
/* A new string of `len` bytes for the caller to fill; chars[len] is NUL. */
PlumString *plum_string_alloc(size_t len);
/* The same with room to grow to `cap` bytes in place. */
PlumString *plum_string_alloc_room(size_t len, size_t cap);
/*
 * Append the strings parts[0..n) to the string in `*slot`, whose new length
 * the caller has checked is at most PLUM_MAX_STRING. When `*slot` is its
 * only owner and it isn't frozen, its buffer grows in place, half as much
 * again each time, and may move; otherwise `*slot` gets a new string
 * (copy-on-write). The parts are only read; one may be the string itself.
 */
void plum_string_append(PlumValue *slot, const PlumValue *parts, size_t n);

/* The number of characters in `s`, counted once and remembered. A string
 * with as many characters as bytes is ASCII, so indexing it is direct. */
size_t plum_string_length(PlumString *s);

/* The byte offset of character `index` (which may equal the length). */
size_t plum_string_offset(PlumString *s, size_t index);

PlumFunction *plum_function_new(const char *name, int arity);

/* A tree's run-time info; `field_names` (interned) is copied. */
PlumTreeInfo *plum_tree_info_new(const char *name, PlumTreeInfo *parent, int field_count,
                                 const char *const *field_names);
/* A new object of `tree`, its fields none until set. */
PlumObject *plum_object_new(PlumTreeInfo *tree);
/* Is `tree` `ancestor` or a tree that extends it? */
bool plum_tree_is(const PlumTreeInfo *tree, const PlumTreeInfo *ancestor);

/* The built-in error trees (spec 10.2). Each has a message; all extend Error. */
#define PLUM_ERRORS(X)                    \
    X(ERROR, "Error")                     \
    X(VALUE, "ValueError")                \
    X(PARSE, "ParseError")                \
    X(INDEX, "IndexError")                \
    X(KEY, "KeyError")                    \
    X(OVERFLOW, "OverflowError")          \
    X(ZERO_DIVISION, "ZeroDivisionError") \
    X(IO, "IOError")                      \
    X(NET, "NetError")                    \
    X(ASSERT, "AssertError")

typedef enum PlumErrorKind {
#define PLUM_ERROR_ENUM(id, name) PLUM_ERROR_##id,
    PLUM_ERRORS(PLUM_ERROR_ENUM)
#undef PLUM_ERROR_ENUM
        PLUM_ERROR_COUNT
} PlumErrorKind;

const char *plum_error_name(PlumErrorKind kind);

/* A cell holding `v` (taking over its reference). */
PlumCell *plum_cell_new(PlumValue v);
/* A closure of `fn` with room for its captures, filled by the caller. */
PlumClosure *plum_closure_new(PlumFunction *fn);

/* An empty list with room for `cap` items, with one reference. */
PlumList *plum_list_new(size_t cap);
/* Append `v`, taking over its reference. The list must not be shared. */
void plum_list_push(PlumList *list, PlumValue v);
/*
 * The list in `*slot`, made safe to change: if another holder shares it,
 * `*slot` gets its own copy first (copy-on-write).
 */
PlumList *plum_list_unique(PlumValue *slot);

/* Structural equality: numbers by value, strings by content. */
bool plum_values_equal(PlumValue a, PlumValue b);

/* Write a value the way `drop` prints it (spec 3.2.1): strings inside a
 * collection are quoted. */
void plum_print_value(FILE *out, PlumValue v);

/* Order two numbers or two strings: <0, 0, >0. */
int plum_compare_values(PlumValue a, PlumValue b);

#endif /* PLUM_VM_VALUE_H */
