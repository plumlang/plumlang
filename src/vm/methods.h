/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The built-in methods of collections, listed once: the type checker gives
 * them types, the compiler numbers them, the VM runs them.
 *
 * X(ID, receiver, name, changes the receiver)
 *
 * A method that changes its receiver, called on a variable (`xs.push(1)`),
 * works on the variable's own value in place, copying it first only if
 * another variable shares it (copy-on-write, spec 11.1).
 */
#ifndef PLUM_VM_METHODS_H
#define PLUM_VM_METHODS_H

#include <stdbool.h>

#define PLUM_METHODS(X)                        \
    X(LIST_PUSH, LIST, "push", true)           \
    X(LIST_POP, LIST, "pop", true)             \
    X(LIST_INSERT, LIST, "insert", true)       \
    X(LIST_REMOVE_AT, LIST, "remove_at", true) \
    X(LIST_CLEAR, LIST, "clear", true)         \
    X(LIST_REVERSE, LIST, "reverse", true)     \
    X(LIST_SORT, LIST, "sort", true)           \
    X(LIST_CONTAINS, LIST, "contains", false)  \
    X(LIST_INDEX_OF, LIST, "index_of", false)  \
    X(LIST_SORTED, LIST, "sorted", false)      \
    X(LIST_JOIN, LIST, "join", false)          \
    X(MAP_GET, MAP, "get", false)              \
    X(MAP_REMOVE, MAP, "remove", true)         \
    X(MAP_KEYS, MAP, "keys", false)            \
    X(MAP_VALUES, MAP, "values", false)        \
    X(MAP_CLEAR, MAP, "clear", true)           \
    X(SET_ADD, SET, "add", true)               \
    X(SET_REMOVE, SET, "remove", true)         \
    X(SET_CONTAINS, SET, "contains", false)    \
    X(SET_CLEAR, SET, "clear", true)

typedef enum PlumMethod {
#define PLUM_METHOD_ENUM(id, receiver, name, mutates) PLUM_METHOD_##id,
    PLUM_METHODS(PLUM_METHOD_ENUM)
#undef PLUM_METHOD_ENUM
        PLUM_METHOD_COUNT
} PlumMethod;

/* The receiver kinds methods belong to. */
typedef enum PlumMethodReceiver {
    PLUM_RECEIVER_LIST,
    PLUM_RECEIVER_MAP,
    PLUM_RECEIVER_SET,
} PlumMethodReceiver;

#include "vm/value.h"

#include <stddef.h>

/*
 * Run method `m`. `receiver` is the slot holding the receiver: a
 * variable's own slot, so a changing method copies a shared list into it
 * first, or a stack slot for a temporary. `args` are borrowed. On success
 * stores the result (with its own reference) and returns true; otherwise
 * writes the reason into `error`.
 */
bool plum_run_method(PlumMethod m, PlumValue *receiver, const PlumValue *args, int argc,
                     PlumValue *result, char *error, size_t error_size);

/* The method `name` of `receiver`, or -1. */
int plum_method_find(PlumMethodReceiver receiver, const char *name);
const char *plum_method_name(PlumMethod m);
bool plum_method_mutates(PlumMethod m);

#endif /* PLUM_VM_METHODS_H */
