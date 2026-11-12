/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Growable arrays of any element type.
 *
 *     PLUM_ARRAY(int) numbers = {0};
 *     plum_array_push(&numbers, 42);
 *     for (size_t i = 0; i < numbers.len; i++) use(numbers.items[i]);
 *     plum_array_free(&numbers);
 *
 * A zero-initialised array is empty and valid. Growth doubles the
 * capacity, with overflow checked by plum_realloc_array.
 */
#ifndef PLUM_UTIL_ARRAY_H
#define PLUM_UTIL_ARRAY_H

#include "util/alloc.h"

#include <stddef.h>

#define PLUM_ARRAY(T) \
    struct {          \
        T *items;     \
        size_t len;   \
        size_t cap;   \
    }

/*
 * Make room for at least `extra` more elements and return the (possibly
 * moved) items pointer. Used by the macros below; returning the pointer
 * instead of writing through a void ** keeps strict aliasing intact.
 */
void *plum_array_grow(void *items, size_t *cap, size_t len, size_t extra, size_t elem_size);

#define plum_array_reserve(arr, extra) \
    ((arr)->items =                    \
         plum_array_grow((arr)->items, &(arr)->cap, (arr)->len, (extra), sizeof(*(arr)->items)))

#define plum_array_push(arr, value)           \
    do {                                      \
        plum_array_reserve((arr), 1);         \
        (arr)->items[(arr)->len++] = (value); \
    } while (0)

/* The last element; the array must not be empty. */
#define plum_array_last(arr) ((arr)->items[(arr)->len - 1])

/* Remove and return the last element; the array must not be empty. */
#define plum_array_pop(arr) ((arr)->items[--(arr)->len])

#define plum_array_clear(arr) ((arr)->len = 0)

#define plum_array_free(arr)     \
    do {                         \
        plum_free((arr)->items); \
        (arr)->items = NULL;     \
        (arr)->len = 0;          \
        (arr)->cap = 0;          \
    } while (0)

#endif /* PLUM_UTIL_ARRAY_H */
