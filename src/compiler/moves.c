/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Moving a local on its last use (#122). Once a function is compiled, a
 * liveness analysis over its bytecode finds each GET_LOCAL after which no
 * path reads the slot again before writing it, and turns it into
 * MOVE_LOCAL: the value leaves the slot (which holds none after) instead
 * of being copied with a new reference. The reference count of a list
 * passed on its last use stays 1, so the function it goes to can change
 * it in place instead of copying it, and nothing is counted or released
 * for it twice.
 *
 * The analysis is conservative. Every instruction that reads a slot is
 * listed in slot_uses(); one that reads slots it doesn't name
 * (MAKE_CLOSURE) counts as reading them all. An error inside a try can
 * jump to its handler from anywhere in the try, so every instruction from
 * a TRY to its handler also leads to the handler. The bytecode is changed
 * only here, at compile time: it is read-only while it runs (#150).
 */
#include "compiler/chunk.h"
#include "util/alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t Word;

typedef struct Insn {
    size_t at;   /* offset of the opcode */
    int succ[2]; /* following instructions (-1: none), besides handlers */
} Insn;

/* A try: from its TRY to its handler, an error may jump to the handler. */
typedef struct Try {
    int from, handler;
} Try;

/* The slots instruction `op` reads (uses) and writes over (def), or *all
 * if it may read any. A def that doesn't happen on every path out of the
 * instruction isn't listed: it would hide a use after it. */
static void slot_uses(PlumOpcode op, uint16_t a, uint16_t b, uint16_t c, int uses[3], int *nuses,
                      int *def, bool *all) {
    *nuses = 0;
    *def = -1;
    *all = false;
    switch (op) {
    case PLUM_OP_SET_LOCAL:
        *def = a;
        return;
    case PLUM_OP_GET_LOCAL:
    case PLUM_OP_MOVE_LOCAL:
    case PLUM_OP_BOX_LOCAL:
    case PLUM_OP_GET_CELL:
    case PLUM_OP_SET_CELL:
    case PLUM_OP_ADD_INT_L:
    case PLUM_OP_SUB_INT_L:
    case PLUM_OP_MUL_INT_L:
    case PLUM_OP_ADD_FLOAT_L:
    case PLUM_OP_SUB_FLOAT_L:
    case PLUM_OP_MUL_FLOAT_L:
    case PLUM_OP_DIV_FLOAT_L:
    case PLUM_OP_ADD_INT_LK:
    case PLUM_OP_SUB_INT_LK:
    case PLUM_OP_MUL_INT_LK:
    case PLUM_OP_FLOORDIV_INT_LK:
    case PLUM_OP_MOD_INT_LK:
    case PLUM_OP_LOCAL_ADD_INT_K:
    case PLUM_OP_LOCAL_SUB_INT_K:
    case PLUM_OP_LOCAL_MUL_INT_K:
    case PLUM_OP_LOCAL_FLOORDIV_INT_K:
    case PLUM_OP_LOCAL_MOD_INT_K:
    case PLUM_OP_APPEND_LOCAL:
    case PLUM_OP_INDEX_LIST_L:
    case PLUM_OP_LEN_L:
    case PLUM_OP_METHOD_LOCAL:
    case PLUM_OP_SET_INDEX_LOCAL:
    case PLUM_OP_SET_PATH_LOCAL:
    case PLUM_OP_METHOD_PATH_LOCAL:
    case PLUM_OP_GET_FIELD_L:
    case PLUM_OP_SET_FIELD_L:
    case PLUM_OP_FIELD_ADD_INT_K:
    case PLUM_OP_FIELD_SUB_INT_K:
    case PLUM_OP_FIELD_ADD_FLOAT_K:
    case PLUM_OP_FIELD_SUB_FLOAT_K:
    case PLUM_OP_JUMP_IF_NOT_LT_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_LE_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_GT_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_GE_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_EQ_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_NE_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_MOD_EQ0_LK:
    case PLUM_OP_JUMP_IF_NOT_MOD_NE0_LK:
    case PLUM_OP_LOOP_IF_LT_INT_LK:
    case PLUM_OP_LOOP_IF_LE_INT_LK:
    case PLUM_OP_LOOP_IF_GT_INT_LK:
    case PLUM_OP_LOOP_IF_GE_INT_LK:
    case PLUM_OP_LOOP_IF_EQ_INT_LK:
    case PLUM_OP_LOOP_IF_NE_INT_LK:
    case PLUM_OP_LOCAL_ADD_INT:
    case PLUM_OP_LOCAL_SUB_INT:
    case PLUM_OP_RETURN_LOCAL:
        uses[(*nuses)++] = a;
        return;
    case PLUM_OP_INDEX_LIST_LL:
    case PLUM_OP_ADD_INT_LL:
    case PLUM_OP_SUB_INT_LL:
    case PLUM_OP_MUL_INT_LL:
    case PLUM_OP_ADD_FLOAT_LL:
    case PLUM_OP_SUB_FLOAT_LL:
    case PLUM_OP_MUL_FLOAT_LL:
    case PLUM_OP_LOCAL_ADD_INT_L:
    case PLUM_OP_LOCAL_SUB_INT_L:
        uses[(*nuses)++] = a;
        uses[(*nuses)++] = b;
        return;
    case PLUM_OP_GLOBAL_ADD_INT_L:
    case PLUM_OP_GLOBAL_SUB_INT_L:
        uses[(*nuses)++] = b;
        return;
    case PLUM_OP_FIELD_ADD_INT_L:
    case PLUM_OP_FIELD_SUB_INT_L:
    case PLUM_OP_FIELD_ADD_FLOAT_L:
    case PLUM_OP_FIELD_SUB_FLOAT_L:
        uses[(*nuses)++] = a;
        uses[(*nuses)++] = c;
        return;
    case PLUM_OP_FOR_RANGE:
    case PLUM_OP_FOR_NEXT:
    case PLUM_OP_FOR_EACH:
    case PLUM_OP_FOR_EACH2:
    case PLUM_OP_FOR_EACH_NEXT:
    case PLUM_OP_FOR_EACH2_NEXT:
        /* the variable a FOR_EACH sets is written only while it goes on */
        uses[(*nuses)++] = a;
        uses[(*nuses)++] = a + 1;
        return;
    case PLUM_OP_MAKE_CLOSURE:
        *all = true; /* the cells of the locals it captures */
        return;
    default:
        return;
    }
}

/* Where the jump of instruction `op` goes, relative to the next
 * instruction: forward (> 0) or back (< 0); 0 if it doesn't jump. Sets
 * *ends when nothing runs after it in this function. */
static long jump_of(PlumOpcode op, uint16_t a, uint16_t b, uint16_t c, bool *ends) {
    *ends = false;
    switch (op) {
    case PLUM_OP_JUMP:
        *ends = true;
        return a;
    case PLUM_OP_LOOP:
        *ends = true;
        return -(long)a;
    case PLUM_OP_JUMP_IF_FALSE:
    case PLUM_OP_JUMP_IF_FALSE_KEEP:
    case PLUM_OP_JUMP_IF_TRUE_KEEP:
    case PLUM_OP_JUMP_IF_NOT_NONE_KEEP:
    case PLUM_OP_JUMP_IF_NONE_KEEP:
    case PLUM_OP_JUMP_IF_NOT_LT_INT:
    case PLUM_OP_JUMP_IF_NOT_LE_INT:
    case PLUM_OP_JUMP_IF_NOT_GT_INT:
    case PLUM_OP_JUMP_IF_NOT_GE_INT:
    case PLUM_OP_JUMP_IF_NOT_EQ_INT:
    case PLUM_OP_JUMP_IF_NOT_NE_INT:
    case PLUM_OP_JUMP_IF_NOT_LT_FLOAT:
    case PLUM_OP_JUMP_IF_NOT_LE_FLOAT:
    case PLUM_OP_JUMP_IF_NOT_GT_FLOAT:
    case PLUM_OP_JUMP_IF_NOT_GE_FLOAT:
    case PLUM_OP_JUMP_IF_NOT_EQ_FLOAT:
    case PLUM_OP_JUMP_IF_NOT_NE_FLOAT:
    case PLUM_OP_TRY:
        return a;
    case PLUM_OP_JUMP_IF_NOT_LT_INT_K:
    case PLUM_OP_JUMP_IF_NOT_LE_INT_K:
    case PLUM_OP_JUMP_IF_NOT_GT_INT_K:
    case PLUM_OP_JUMP_IF_NOT_GE_INT_K:
    case PLUM_OP_JUMP_IF_NOT_EQ_INT_K:
    case PLUM_OP_JUMP_IF_NOT_NE_INT_K:
    case PLUM_OP_JUMP_IF_NOT_LT_FLOAT_K:
    case PLUM_OP_JUMP_IF_NOT_LE_FLOAT_K:
    case PLUM_OP_JUMP_IF_NOT_GT_FLOAT_K:
    case PLUM_OP_JUMP_IF_NOT_GE_FLOAT_K:
    case PLUM_OP_JUMP_IF_NOT_EQ_FLOAT_K:
    case PLUM_OP_JUMP_IF_NOT_NE_FLOAT_K:
        return b;
    case PLUM_OP_JUMP_IF_NOT_LT_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_LE_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_GT_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_GE_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_EQ_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_NE_INT_LK:
    case PLUM_OP_JUMP_IF_NOT_MOD_EQ0_LK:
    case PLUM_OP_JUMP_IF_NOT_MOD_NE0_LK:
    case PLUM_OP_FOR_RANGE:
    case PLUM_OP_FOR_EACH:
    case PLUM_OP_FOR_EACH2:
        return c;
    case PLUM_OP_FOR_NEXT:
    case PLUM_OP_LOOP_IF_LT_INT_LK:
    case PLUM_OP_LOOP_IF_LE_INT_LK:
    case PLUM_OP_LOOP_IF_GT_INT_LK:
    case PLUM_OP_LOOP_IF_GE_INT_LK:
    case PLUM_OP_LOOP_IF_EQ_INT_LK:
    case PLUM_OP_LOOP_IF_NE_INT_LK:
    case PLUM_OP_FOR_EACH_NEXT:
    case PLUM_OP_FOR_EACH2_NEXT:
        return -(long)c;
    case PLUM_OP_RETURN:
    case PLUM_OP_RETURN_LOCAL:
    case PLUM_OP_RETURN_NONE:
    case PLUM_OP_RETURN_ADD_INT:
    case PLUM_OP_TAIL_SELF: /* sets every slot from its arguments */
    case PLUM_OP_THROW:
        *ends = true;
        return 0;
    default:
        return 0;
    }
}

/* Read the three operands of the instruction at `at` (0 if it has fewer). */
static void operands(const uint8_t *code, size_t at, uint16_t *a, uint16_t *b, uint16_t *c) {
    int n = plum_opcode_operands((PlumOpcode)code[at]);
    *a = n >= 1 ? plum_chunk_read_u16(&code[at + 1]) : 0;
    *b = n >= 2 ? plum_chunk_read_u16(&code[at + 3]) : 0;
    *c = n >= 3 ? plum_chunk_read_u16(&code[at + 5]) : 0;
}

static bool has(const Word *set, int slot) {
    return (set[slot / 64] >> (slot % 64)) & 1u;
}

/* The slots live after instruction i: live into a following instruction,
 * or into the handler of a try it is in. */
static void live_after(const uint8_t *code, const Insn *insns, size_t i, const Try *tries,
                       size_t try_count, const Word *live_in, size_t words, Word *out) {
    memset(out, 0, words * sizeof(Word));
    for (int s = 0; s < 2; s++) {
        int to = insns[i].succ[s];
        for (size_t w = 0; to >= 0 && w < words; w++) {
            out[w] |= live_in[(size_t)to * words + w];
        }
    }
    /* FOR_EACH sets its variable (two with FOR_EACH2) when it goes on into
     * the body, its first successor; the slot isn't live just before.
     * FOR_EACH_NEXT goes on into the body by its jump, its second. */
    PlumOpcode op = (PlumOpcode)code[insns[i].at];
    bool each = op == PLUM_OP_FOR_EACH || op == PLUM_OP_FOR_EACH2;
    bool next = op == PLUM_OP_FOR_EACH_NEXT || op == PLUM_OP_FOR_EACH2_NEXT;
    if ((each || next) && insns[i].succ[each ? 0 : 1] >= 0) {
        int var = plum_chunk_read_u16(&code[insns[i].at + 3]);
        int to = insns[i].succ[each ? 1 : 0];
        bool two = op == PLUM_OP_FOR_EACH2 || op == PLUM_OP_FOR_EACH2_NEXT;
        for (int v = var; v <= var + two; v++) {
            if ((size_t)v < words * 64 && !(to >= 0 && has(&live_in[(size_t)to * words], v))) {
                out[v / 64] &= ~((Word)1 << (v % 64));
            }
        }
    }
    for (size_t t = 0; t < try_count; t++) {
        if (tries[t].from <= (int)i && (int)i < tries[t].handler) {
            for (size_t w = 0; w < words; w++) {
                out[w] |= live_in[(size_t)tries[t].handler * words + w];
            }
        }
    }
}

void plum_chunk_moves(PlumChunk *chunk, int slot_count, const uint8_t *movable,
                      size_t movable_count) {
    uint8_t *code = chunk->code.items;
    size_t len = chunk->code.len;
    if (slot_count <= 0 || len == 0) {
        return;
    }
    /* The instructions, and which one starts at each offset. */
    int *index = plum_alloc(len * sizeof(int));
    size_t count = 0;
    for (size_t at = 0; at < len; at += 1 + 2 * (size_t)plum_opcode_operands(code[at])) {
        index[at] = (int)count++;
    }
    Insn *insns = plum_alloc_zeroed(count, sizeof(Insn));
    size_t k = 0;
    bool any_get = false;
    for (size_t at = 0; at < len; at += 1 + 2 * (size_t)plum_opcode_operands(code[at])) {
        PlumOpcode op = (PlumOpcode)code[at];
        uint16_t a, b, c;
        operands(code, at, &a, &b, &c);
        size_t next = at + 1 + 2 * (size_t)plum_opcode_operands(op);
        bool ends;
        long d = jump_of(op, a, b, c, &ends);
        Insn *in = &insns[k++];
        in->at = at;
        in->succ[0] = in->succ[1] = -1;
        int s = 0;
        if (!ends && next < len) {
            in->succ[s++] = index[next];
        }
        if (d != 0 && (long)next + d >= 0 && (size_t)((long)next + d) < len) {
            in->succ[s++] = index[(size_t)((long)next + d)];
        }
        any_get = any_get || op == PLUM_OP_GET_LOCAL;
    }
    if (!any_get) {
        plum_free(insns);
        plum_free(index);
        return;
    }
    /* Everything from a TRY to its handler may jump to the handler (and
     * from there, if it throws again, to an outer one's). */
    Try *tries = NULL;
    size_t try_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (code[insns[i].at] == PLUM_OP_TRY) {
            size_t target = insns[i].at + 3 + plum_chunk_read_u16(&code[insns[i].at + 1]);
            if (target < len) {
                tries = plum_realloc_array(tries, try_count + 1, sizeof(Try));
                tries[try_count++] = (Try){(int)i, index[target]};
            }
        }
    }

    size_t words = ((size_t)slot_count + 63) / 64;
    Word *live_in = plum_alloc_zeroed(count * words, sizeof(Word));
    Word *out = plum_alloc(words * sizeof(Word));
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = count; i-- > 0;) {
            const Insn *in = &insns[i];
            live_after(code, insns, i, tries, try_count, live_in, words, out);
            PlumOpcode op = (PlumOpcode)code[in->at];
            uint16_t a, b, c;
            operands(code, in->at, &a, &b, &c);
            int uses[3], nuses, def;
            bool all;
            slot_uses(op, a, b, c, uses, &nuses, &def, &all);
            if (def >= 0 && def < slot_count) {
                out[def / 64] &= ~((Word)1 << (def % 64));
            }
            for (int u = 0; u < nuses; u++) {
                if (uses[u] < slot_count) {
                    out[uses[u] / 64] |= (Word)1 << (uses[u] % 64);
                }
            }
            if (all) {
                memset(out, 0xFF, words * sizeof(Word));
            }
            Word *in_set = &live_in[i * words];
            if (memcmp(in_set, out, words * sizeof(Word)) != 0) {
                memcpy(in_set, out, words * sizeof(Word));
                changed = true;
            }
        }
    }

    /* A GET_LOCAL whose slot no successor reads becomes a move. */
    for (size_t i = 0; i < count; i++) {
        const Insn *in = &insns[i];
        if (code[in->at] != PLUM_OP_GET_LOCAL) {
            continue;
        }
        int slot = plum_chunk_read_u16(&code[in->at + 1]);
        if (slot >= slot_count || (size_t)slot >= movable_count || movable[slot] != 1) {
            continue;
        }
        live_after(code, insns, i, tries, try_count, live_in, words, out);
        if (!has(out, slot)) {
            code[in->at] = PLUM_OP_MOVE_LOCAL;
        }
    }
    plum_free(tries);
    plum_free(out);
    plum_free(live_in);
    plum_free(insns);
    plum_free(index);
}
