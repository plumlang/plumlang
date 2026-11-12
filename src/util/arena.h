/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Arena allocator for data that lives as long as one compilation: tokens,
 * syntax trees, types. Allocation is a pointer bump; everything is freed
 * at once by plum_arena_free. Every pointer is aligned for any C type.
 */
#ifndef PLUM_UTIL_ARENA_H
#define PLUM_UTIL_ARENA_H

#include <stddef.h>

typedef struct PlumArenaChunk PlumArenaChunk;

typedef struct PlumArena {
    PlumArenaChunk *head; /* the chunk being filled; older chunks follow */
    size_t bytes_used;    /* bytes handed out, for statistics */
    size_t bytes_reserved;
} PlumArena;

void plum_arena_init(PlumArena *arena);
void *plum_arena_alloc(PlumArena *arena, size_t size);
void *plum_arena_alloc_zeroed(PlumArena *arena, size_t size);
/* A NUL-terminated copy of s[0..len) in the arena. */
char *plum_arena_strndup(PlumArena *arena, const char *s, size_t len);
void plum_arena_free(PlumArena *arena);

#endif /* PLUM_UTIL_ARENA_H */
