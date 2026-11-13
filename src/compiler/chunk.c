/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "compiler/chunk.h"

#include "util/fmt.h"

#include <string.h>

static const char *const opcode_names[] = {
#define PLUM_OPCODE_NAME(name, operands, doc) #name,
    PLUM_OPCODES(PLUM_OPCODE_NAME)
#undef PLUM_OPCODE_NAME
};

static const int opcode_operands[] = {
#define PLUM_OPCODE_OPERANDS(name, operands, doc) operands,
    PLUM_OPCODES(PLUM_OPCODE_OPERANDS)
#undef PLUM_OPCODE_OPERANDS
};

static const char *const builtin_names[] = {
#define PLUM_BUILTIN_NAME(name, text) text,
    PLUM_BUILTINS(PLUM_BUILTIN_NAME)
#undef PLUM_BUILTIN_NAME
};

const char *plum_opcode_name(PlumOpcode op) {
    return (unsigned)op < PLUM_OPCODE_COUNT ? opcode_names[op] : "?";
}

int plum_opcode_operands(PlumOpcode op) {
    return (unsigned)op < PLUM_OPCODE_COUNT ? opcode_operands[op] : 0;
}

const char *plum_builtin_name(PlumBuiltin b) {
    return (unsigned)b < PLUM_BUILTIN_COUNT ? builtin_names[b] : "?";
}

void plum_chunk_init(PlumChunk *chunk) {
    memset(chunk, 0, sizeof *chunk);
}

void plum_chunk_free(PlumChunk *chunk) {
    for (size_t i = 0; i < chunk->constants.len; i++) {
        plum_release(chunk->constants.items[i]);
    }
    plum_array_free(&chunk->code);
    plum_array_free(&chunk->constants);
    plum_array_free(&chunk->lines);
}

static void note_line(PlumChunk *chunk, uint32_t line) {
    if (chunk->lines.len == 0 || plum_array_last(&chunk->lines).line != line) {
        PlumLineRun run = {(uint32_t)chunk->code.len, line};
        plum_array_push(&chunk->lines, run);
    }
}

void plum_chunk_write_op(PlumChunk *chunk, PlumOpcode op, uint32_t line) {
    note_line(chunk, line);
    plum_array_push(&chunk->code, (uint8_t)op);
}

void plum_chunk_write_u16(PlumChunk *chunk, uint16_t value, uint32_t line) {
    note_line(chunk, line);
    plum_array_push(&chunk->code, (uint8_t)(value & 0xFF));
    plum_array_push(&chunk->code, (uint8_t)(value >> 8));
}

size_t plum_chunk_add_constant(PlumChunk *chunk, PlumValue value) {
    for (size_t i = 0; i < chunk->constants.len; i++) {
        PlumValue c = chunk->constants.items[i];
        bool same_kind = c.type == value.type &&
                         (c.type != PLUM_VAL_OBJ ||
                          (plum_is_obj(c, PLUM_OBJ_STRING) && plum_is_obj(value, PLUM_OBJ_STRING)));
        bool same_float_bits =
            c.type != PLUM_VAL_FLOAT || memcmp(&c.as.f, &value.as.f, sizeof(double)) == 0;
        if (same_kind && same_float_bits && plum_values_equal(c, value)) {
            plum_release(value);
            return i;
        }
    }
    plum_array_push(&chunk->constants, value);
    return chunk->constants.len - 1;
}

uint32_t plum_chunk_line(const PlumChunk *chunk, size_t offset) {
    uint32_t line = 0;
    for (size_t i = 0; i < chunk->lines.len && chunk->lines.items[i].offset <= offset; i++) {
        line = chunk->lines.items[i].line;
    }
    return line;
}

static void print_constant(FILE *out, PlumValue v) {
    if (plum_is_obj(v, PLUM_OBJ_STRING)) {
        plum_write_quoted(out, plum_as_string(v)->chars, plum_as_string(v)->len);
    } else {
        plum_print_value(out, v);
    }
}

static void disassemble_chunk(FILE *out, const PlumFunction *fn) {
    const PlumChunk *chunk = fn->chunk;
    fprintf(out, "== %s (%d parameter%s, %d slot%s) ==\n", fn->name, fn->arity,
            fn->arity == 1 ? "" : "s", fn->slot_count, fn->slot_count == 1 ? "" : "s");
    uint32_t last_line = 0;
    for (size_t at = 0; at < chunk->code.len;) {
        PlumOpcode op = (PlumOpcode)chunk->code.items[at];
        uint32_t line = plum_chunk_line(chunk, at);
        if (line != last_line) {
            fprintf(out, "%04zu %4u  ", at, (unsigned)line);
            last_line = line;
        } else {
            fprintf(out, "%04zu    |  ", at);
        }
        fputs(plum_opcode_name(op), out);
        int n = plum_opcode_operands(op);
        uint16_t a = n >= 1 ? plum_chunk_read_u16(&chunk->code.items[at + 1]) : 0;
        uint16_t b = n >= 2 ? plum_chunk_read_u16(&chunk->code.items[at + 3]) : 0;
        uint16_t c3 = n >= 3 ? plum_chunk_read_u16(&chunk->code.items[at + 5]) : 0;
        size_t next = at + 1 + 2 * (size_t)n;
        switch (op) {
        case PLUM_OP_CONST:
            fprintf(out, " %u  ; ", a);
            print_constant(out, chunk->constants.items[a]);
            break;
        case PLUM_OP_TO_STRING:
            if (a == 0xFFFF) {
                fputs(" -", out);
            } else {
                fprintf(out, " %u  ; ", a);
                print_constant(out, chunk->constants.items[a]);
            }
            break;
        case PLUM_OP_JUMP:
        case PLUM_OP_JUMP_IF_FALSE:
        case PLUM_OP_JUMP_IF_FALSE_KEEP:
        case PLUM_OP_JUMP_IF_TRUE_KEEP:
        case PLUM_OP_JUMP_IF_NOT_NONE_KEEP:
            fprintf(out, " %u  ; -> %04zu", a, next + a);
            break;
        case PLUM_OP_LOOP:
            fprintf(out, " %u  ; -> %04zu", a, next - a);
            break;
        case PLUM_OP_FOR_RANGE:
            fprintf(out, " %u %u %u  ; local %u %s local %u, else -> %04zu", a, b, c3, a,
                    b ? "<=" : "<", a + 1u, next + c3);
            break;
        case PLUM_OP_FOR_NEXT:
            fprintf(out, " %u %u %u  ; -> %04zu", a, b, c3, next - c3);
            break;
        case PLUM_OP_CALL_BUILTIN:
            fprintf(out, " %s %u", plum_builtin_name((PlumBuiltin)a), b);
            break;
        default:
            for (int i = 0; i < n; i++) {
                fprintf(out, " %u", i == 0 ? a : i == 1 ? b : c3);
            }
            break;
        }
        fputc('\n', out);
        at = next;
    }
}

void plum_disassemble(FILE *out, const PlumFunction *fn) {
    disassemble_chunk(out, fn);
    for (size_t i = 0; i < fn->chunk->constants.len; i++) {
        PlumValue c = fn->chunk->constants.items[i];
        if (plum_is_obj(c, PLUM_OBJ_FUNCTION)) {
            fputc('\n', out);
            plum_disassemble(out, plum_as_function(c));
        }
    }
}
