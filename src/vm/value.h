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
} PlumString;

typedef struct PlumChunk PlumChunk;

typedef struct PlumFunction {
    PlumObj obj;
    const char *name; /* for stack traces; "<main>" for the top level */
    int arity;
    int slot_count;   /* parameters + locals */
    PlumChunk *chunk; /* owned */
} PlumFunction;

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

PlumFunction *plum_function_new(const char *name, int arity);

/* Structural equality: numbers by value, strings by content. */
bool plum_values_equal(PlumValue a, PlumValue b);

/* Write a value the way `drop` prints it (spec 3.2.1). */
void plum_print_value(FILE *out, PlumValue v);

#endif /* PLUM_VM_VALUE_H */
