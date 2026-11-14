/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
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
} PlumObjType;

/* The header every heap object starts with. */
typedef struct PlumObj {
    uint32_t refcount;
    uint8_t type; /* PlumObjType */
} PlumObj;

typedef struct PlumValue {
    PlumValueType type;
    union {
        bool b;
        int64_t i;
        double f;
        PlumObj *obj;
    } as;
} PlumValue;

/* Immutable UTF-8 text. `chars` points just past the struct, NUL-terminated. */
typedef struct PlumString {
    PlumObj obj;
    size_t len;
    char *chars;
    size_t char_count; /* characters (code points); valid once `counted` */
    bool counted;
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
} PlumHandle;

static inline PlumHandle *plum_as_handle(PlumValue v) {
    return (PlumHandle *)(void *)v.as.obj;
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

static inline PlumValue plum_int(int64_t i) {
    PlumValue v;
    v.type = PLUM_VAL_INT;
    v.as.i = i;
    return v;
}

static inline PlumValue plum_float(double f) {
    PlumValue v;
    v.type = PLUM_VAL_FLOAT;
    v.as.f = f;
    return v;
}

static inline PlumValue plum_obj(PlumObj *obj) {
    PlumValue v;
    v.type = PLUM_VAL_OBJ;
    v.as.obj = obj;
    return v;
}

static inline bool plum_is_obj(PlumValue v, PlumObjType type) {
    return v.type == PLUM_VAL_OBJ && v.as.obj->type == type;
}

static inline PlumString *plum_as_string(PlumValue v) {
    return (PlumString *)v.as.obj;
}

static inline PlumFunction *plum_as_function(PlumValue v) {
    return (PlumFunction *)v.as.obj;
}

static inline PlumCell *plum_as_cell(PlumValue v) {
    return (PlumCell *)v.as.obj;
}

static inline PlumClosure *plum_as_closure(PlumValue v) {
    return (PlumClosure *)v.as.obj;
}

/* A function or closure value, either of which can be called. */
static inline bool plum_is_callable(PlumValue v) {
    return v.type == PLUM_VAL_OBJ &&
           (v.as.obj->type == PLUM_OBJ_FUNCTION || v.as.obj->type == PLUM_OBJ_CLOSURE);
}

static inline PlumTreeInfo *plum_as_tree_info(PlumValue v) {
    return (PlumTreeInfo *)v.as.obj;
}

static inline PlumObject *plum_as_object(PlumValue v) {
    return (PlumObject *)v.as.obj;
}

static inline PlumList *plum_as_list(PlumValue v) {
    return (PlumList *)v.as.obj;
}

/* Take another reference to an object value (no-op for the rest). */
static inline PlumValue plum_retain(PlumValue v) {
    if (v.type == PLUM_VAL_OBJ) {
        v.as.obj->refcount++;
    }
    return v;
}

/* Give up a reference; frees the object when it was the last one. */
void plum_release(PlumValue v);

/* A new string holding a copy of s[0..len), with one reference. */
PlumString *plum_string_new(const char *s, size_t len);
/* A new string of `len` bytes for the caller to fill; chars[len] is NUL. */
PlumString *plum_string_alloc(size_t len);

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
