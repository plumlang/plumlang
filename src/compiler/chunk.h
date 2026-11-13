/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * A chunk is one function's compiled code: its instructions, the constants
 * they use, and the source line of every instruction (for errors and stack
 * traces).
 */
#ifndef PLUM_COMPILER_CHUNK_H
#define PLUM_COMPILER_CHUNK_H

#include "compiler/opcodes.h"
#include "util/array.h"
#include "vm/value.h"

#include <stdint.h>
#include <stdio.h>

/* A run of instructions on one source line. */
typedef struct PlumLineRun {
    uint32_t offset; /* first byte of the run */
    uint32_t line;
} PlumLineRun;

struct PlumChunk {
    PLUM_ARRAY(uint8_t) code;
    PLUM_ARRAY(PlumValue) constants; /* each holds a reference */
    PLUM_ARRAY(PlumLineRun) lines;
};

void plum_chunk_init(PlumChunk *chunk);
void plum_chunk_free(PlumChunk *chunk);

/* Append an opcode or operand, recording `line`. */
void plum_chunk_write_op(PlumChunk *chunk, PlumOpcode op, uint32_t line);
void plum_chunk_write_u16(PlumChunk *chunk, uint16_t value, uint32_t line);

static inline uint16_t plum_chunk_read_u16(const uint8_t *at) {
    return (uint16_t)(at[0] | (at[1] << 8));
}

/* Add a constant (taking over its reference); returns its index. Strings
 * and numbers already in the pool are reused. */
size_t plum_chunk_add_constant(PlumChunk *chunk, PlumValue value);

/* The source line of the instruction at `offset`. */
uint32_t plum_chunk_line(const PlumChunk *chunk, size_t offset);

/* Print the instructions of `fn` and every function in its constants. */
void plum_disassemble(FILE *out, const PlumFunction *fn);

#endif /* PLUM_COMPILER_CHUNK_H */
