/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void plum_fatal(const char *what) {
    fprintf(stderr, "plum: fatal: %s\n", what);
    fflush(stderr);
    abort();
}

static void out_of_memory(size_t size) {
    fprintf(stderr, "plum: fatal: out of memory (asked for %zu bytes)\n", size);
    fflush(stderr);
    abort();
}

size_t plum_mul_size(size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) {
        plum_fatal("allocation size overflows");
    }
    return count * size;
}

size_t plum_add_size(size_t a, size_t b) {
    if (a > SIZE_MAX - b) {
        plum_fatal("allocation size overflows");
    }
    return a + b;
}

void *plum_alloc(size_t size) {
    /* malloc(0) may return NULL; always hand back a real pointer. */
    void *p = malloc(size == 0 ? 1 : size);
    if (p == NULL) {
        out_of_memory(size);
    }
    return p;
}

void *plum_alloc_zeroed(size_t count, size_t size) {
    size_t total = plum_mul_size(count, size);
    void *p = calloc(total == 0 ? 1 : total, 1);
    if (p == NULL) {
        out_of_memory(total);
    }
    return p;
}

void *plum_realloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size == 0 ? 1 : size);
    if (p == NULL) {
        out_of_memory(size);
    }
    return p;
}

void *plum_realloc_array(void *ptr, size_t count, size_t size) {
    return plum_realloc(ptr, plum_mul_size(count, size));
}

void plum_free(void *ptr) {
    free(ptr);
}

char *plum_strndup(const char *s, size_t len) {
    char *copy = plum_alloc(plum_add_size(len, 1));
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *plum_strdup(const char *s) {
    return plum_strndup(s, strlen(s));
}
