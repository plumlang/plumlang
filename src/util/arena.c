/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/arena.h"

#include "util/alloc.h"

#include <stdint.h>
#include <string.h>

#define PLUM_ARENA_CHUNK_SIZE ((size_t)64 * 1024)

/*
 * A chunk is this header followed by its data, which starts at the header
 * size rounded up to PLUM_MAX_ALIGN (no flexible array member, which MSVC
 * treats as an extension).
 */
struct PlumArenaChunk {
    PlumArenaChunk *next;
    size_t capacity; /* bytes of data */
    size_t used;
};

static size_t align_up(size_t n) {
    return plum_add_size(n, PLUM_MAX_ALIGN - 1) & ~(PLUM_MAX_ALIGN - 1);
}

static unsigned char *chunk_data(PlumArenaChunk *chunk) {
    return (unsigned char *)chunk + align_up(sizeof(PlumArenaChunk));
}

static PlumArenaChunk *new_chunk(PlumArena *arena, size_t capacity) {
    /* malloc returns memory aligned for any basic type, so the data is too. */
    PlumArenaChunk *chunk = plum_alloc(plum_add_size(align_up(sizeof(PlumArenaChunk)), capacity));
    chunk->next = NULL;
    chunk->capacity = capacity;
    chunk->used = 0;
    arena->bytes_reserved += capacity;
    return chunk;
}

void plum_arena_init(PlumArena *arena) {
    arena->head = NULL;
    arena->bytes_used = 0;
    arena->bytes_reserved = 0;
}

void *plum_arena_alloc(PlumArena *arena, size_t size) {
    size_t need = align_up(size == 0 ? 1 : size);
    PlumArenaChunk *head = arena->head;

    if (head == NULL || head->capacity - head->used < need) {
        if (need > PLUM_ARENA_CHUNK_SIZE / 4) {
            /* A large block gets a chunk of its own, placed behind the
             * current one so the current chunk keeps filling. */
            PlumArenaChunk *big = new_chunk(arena, need);
            big->used = need;
            if (head == NULL) {
                arena->head = big;
            } else {
                big->next = head->next;
                head->next = big;
            }
            arena->bytes_used += need;
            return chunk_data(big);
        }
        PlumArenaChunk *fresh = new_chunk(arena, PLUM_ARENA_CHUNK_SIZE);
        fresh->next = head;
        arena->head = head = fresh;
    }

    void *p = chunk_data(head) + head->used;
    head->used += need;
    arena->bytes_used += need;
    return p;
}

void *plum_arena_alloc_zeroed(PlumArena *arena, size_t size) {
    void *p = plum_arena_alloc(arena, size);
    memset(p, 0, size);
    return p;
}

char *plum_arena_strndup(PlumArena *arena, const char *s, size_t len) {
    char *copy = plum_arena_alloc(arena, plum_add_size(len, 1));
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

void plum_arena_free(PlumArena *arena) {
    PlumArenaChunk *chunk = arena->head;
    while (chunk != NULL) {
        PlumArenaChunk *next = chunk->next;
        plum_free(chunk);
        chunk = next;
    }
    plum_arena_init(arena);
}
