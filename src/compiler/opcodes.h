/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The instruction set (docs/bytecode.md). Each instruction is a one-byte
 * opcode followed by its operands, each a little-endian 16-bit number.
 *
 * The type checker has already proven every operand's type, so arithmetic
 * and comparisons come in typed versions (ADD_INT, ADD_FLOAT) and the VM
 * never inspects types to decide what to do.
 *
 * X(name, operand count, what it does)
 */
#ifndef PLUM_COMPILER_OPCODES_H
#define PLUM_COMPILER_OPCODES_H

#define PLUM_OPCODES(X)                                                                          \
    /* Constants and the stack */                                                                \
    X(CONST, 1, "push constant [a]")                                                             \
    X(NONE, 0, "push none")                                                                      \
    X(TRUE, 0, "push good")                                                                      \
    X(FALSE, 0, "push bad")                                                                      \
    X(POP, 0, "drop the top value")                                                              \
    X(DUP, 0, "push a copy of the top value")                                                    \
    /* Variables */                                                                              \
    X(GET_LOCAL, 1, "push local slot [a]")                                                       \
    X(SET_LOCAL, 1, "pop into local slot [a]")                                                   \
    X(GET_GLOBAL, 1, "push global slot [a]")                                                     \
    X(SET_GLOBAL, 1, "pop into global slot [a]")                                                 \
    /* Integers: overflow and division by zero are runtime errors */                             \
    X(ADD_INT, 0, "a + b")                                                                       \
    X(SUB_INT, 0, "a - b")                                                                       \
    X(MUL_INT, 0, "a * b")                                                                       \
    X(FLOORDIV_INT, 0, "a // b")                                                                 \
    X(MOD_INT, 0, "a % b, with the sign of b")                                                   \
    X(POW_INT, 0, "a ** b, b >= 0")                                                              \
    X(NEG_INT, 0, "-a")                                                                          \
    X(BAND, 0, "a & b")                                                                          \
    X(BOR, 0, "a | b")                                                                           \
    X(BXOR, 0, "a ^ b")                                                                          \
    X(BNOT, 0, "~a")                                                                             \
    X(SHL, 0, "a << b, 0 <= b <= 63")                                                            \
    X(SHR, 0, "a >> b (arithmetic), 0 <= b <= 63")                                               \
    X(LT_INT, 0, "a < b")                                                                        \
    X(LE_INT, 0, "a <= b")                                                                       \
    /* Floats: IEEE 754 */                                                                       \
    X(ADD_FLOAT, 0, "a + b")                                                                     \
    X(SUB_FLOAT, 0, "a - b")                                                                     \
    X(MUL_FLOAT, 0, "a * b")                                                                     \
    X(DIV_FLOAT, 0, "a / b")                                                                     \
    X(FLOORDIV_FLOAT, 0, "floor(a / b)")                                                         \
    X(MOD_FLOAT, 0, "a % b, with the sign of b")                                                 \
    X(POW_FLOAT, 0, "a ** b")                                                                    \
    X(NEG_FLOAT, 0, "-a")                                                                        \
    X(LT_FLOAT, 0, "a < b")                                                                      \
    X(LE_FLOAT, 0, "a <= b")                                                                     \
    X(INT_TO_FLOAT, 0, "convert the top value")                                                  \
    X(INT_TO_FLOAT_2, 0, "convert the value below the top")                                      \
    /* Strings */                                                                                \
    X(CONCAT, 0, "a + b for strings")                                                            \
    X(LT_STR, 0, "a < b by code point")                                                          \
    X(LE_STR, 0, "a <= b by code point")                                                         \
    X(TO_STRING, 1, "text of the top value, with format constant [a] (0xFFFF: none)")            \
    X(BUILD_STRING, 1, "join the top [a] strings")                                               \
    /* Every type */                                                                             \
    X(EQ, 0, "a == b")                                                                           \
    X(NE, 0, "a != b")                                                                           \
    X(NOT, 0, "not a")                                                                           \
    /* Control flow: jumps are relative to the next instruction */                               \
    X(JUMP, 1, "jump forward [a] bytes")                                                         \
    X(JUMP_IF_FALSE, 1, "pop; jump forward [a] bytes if bad")                                    \
    X(JUMP_IF_FALSE_KEEP, 1, "jump forward [a] bytes if bad, keeping the value")                 \
    X(JUMP_IF_TRUE_KEEP, 1, "jump forward [a] bytes if good, keeping the value")                 \
    X(LOOP, 1, "jump back [a] bytes")                                                            \
    X(FOR_RANGE, 2, "local [a] < local [a+1] (or <= if [b]): continue, else jump past the loop") \
    /* Calls */                                                                                  \
    X(CALL, 1, "call the function under [a] arguments")                                          \
    X(CALL_BUILTIN, 2, "call built-in [a] with [b] arguments")                                   \
    X(RETURN, 0, "return the top value")                                                         \
    X(RETURN_NONE, 0, "return none")

typedef enum PlumOpcode {
#define PLUM_OPCODE_ENUM(name, operands, doc) PLUM_OP_##name,
    PLUM_OPCODES(PLUM_OPCODE_ENUM)
#undef PLUM_OPCODE_ENUM
        PLUM_OPCODE_COUNT
} PlumOpcode;

/* The built-in functions CALL_BUILTIN calls. */
#define PLUM_BUILTINS(X)  \
    X(DROP, "drop")       \
    X(LEN, "len")         \
    X(STR, "str")         \
    X(INT, "int")         \
    X(FLOAT, "float")     \
    X(BYTE, "byte")       \
    X(TYPE_OF, "type_of") \
    X(ASSERT, "assert")   \
    X(EXIT, "exit")       \
    X(ASK, "ask")

typedef enum PlumBuiltin {
#define PLUM_BUILTIN_ENUM(name, text) PLUM_BUILTIN_##name,
    PLUM_BUILTINS(PLUM_BUILTIN_ENUM)
#undef PLUM_BUILTIN_ENUM
        PLUM_BUILTIN_COUNT
} PlumBuiltin;

const char *plum_opcode_name(PlumOpcode op);
int plum_opcode_operands(PlumOpcode op);
const char *plum_builtin_name(PlumBuiltin b);

#endif /* PLUM_COMPILER_OPCODES_H */
