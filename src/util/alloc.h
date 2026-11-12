/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Checked allocation (PREAMBLE 3.4). These never return NULL: running out
 * of memory, or asking for a size that overflows size_t, prints a fatal
 * error and aborts. Code that uses them has no NULL checks to forget.
 */
#ifndef PLUM_UTIL_ALLOC_H
#define PLUM_UTIL_ALLOC_H

#include <stddef.h>

void *plum_alloc(size_t size);
void *plum_alloc_zeroed(size_t count, size_t size);
void *plum_realloc(void *ptr, size_t size);
/* Grow an array of `count` elements of `size` bytes; checks count * size. */
void *plum_realloc_array(void *ptr, size_t count, size_t size);
void plum_free(void *ptr);

char *plum_strdup(const char *s);
char *plum_strndup(const char *s, size_t len);

/* count * size, or a fatal error if it overflows. */
size_t plum_mul_size(size_t count, size_t size);
/* a + b, or a fatal error if it overflows. */
size_t plum_add_size(size_t a, size_t b);

/* Print "plum: fatal: <what>" and abort. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#elif defined(_MSC_VER)
__declspec(noreturn)
#endif
void plum_fatal(const char *what);

#endif /* PLUM_UTIL_ALLOC_H */
