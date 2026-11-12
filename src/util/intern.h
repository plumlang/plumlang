/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * String interning: each distinct string is stored once, so two interned
 * strings are equal exactly when their pointers are. The lexer interns
 * every identifier; the checker and VM then compare names by pointer.
 *
 * Interned strings are NUL-terminated and live until plum_interner_free.
 */
#ifndef PLUM_UTIL_INTERN_H
#define PLUM_UTIL_INTERN_H

#include "util/arena.h"
#include "util/map.h"

#include <stddef.h>

typedef struct PlumInterner {
    PlumArena arena; /* the string bytes */
    PlumMap table;   /* interned string -> itself */
} PlumInterner;

void plum_interner_init(PlumInterner *in);
void plum_interner_free(PlumInterner *in);

/* The interned copy of s[0..len). */
const char *plum_intern(PlumInterner *in, const char *s, size_t len);
/* The same, for a NUL-terminated string. */
const char *plum_intern_cstr(PlumInterner *in, const char *s);

/* Number of distinct strings interned. */
size_t plum_interner_count(const PlumInterner *in);

#endif /* PLUM_UTIL_INTERN_H */
