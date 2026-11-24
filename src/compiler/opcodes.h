/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The instruction set (docs/bytecode.md). Each instruction is a one-byte
 * opcode followed by its operands, each a little-endian 16-bit number.
 *
 * The type checker has already proven every operand's type, so arithmetic
 * and comparisons come in typed versions (ADD_INT, ADD_FLOAT) and the VM
 * never inspects types to decide what to do.
 *
 * Some take an operand from the instruction instead of the stack (#118):
 * _K a constant, _L a local, _LK both; LOCAL_ and GLOBAL_ change a
 * variable in place; JUMP_IF_NOT_ compares and jumps (docs/bytecode.md).
 *
 * Fields have fixed positions, the parent tree's first; GET_FIELD_L and
 * the FIELD_ forms work on the object in a local (#119). INVOKE finds a
 * method in the object's tree's table; CALL_METHOD calls one the compiler
 * resolved. Code never changes once compiled: no inline caches.
 *
 * X(name, operand count, what it does)
 */
#ifndef PLUM_COMPILER_OPCODES_H
#define PLUM_COMPILER_OPCODES_H

#define PLUM_OPCODES(X)                                                                            \
    /* Constants and the stack */                                                                  \
    X(CONST, 1, "push constant [a]")                                                               \
    X(NONE, 0, "push none")                                                                        \
    X(TRUE, 0, "push good")                                                                        \
    X(FALSE, 0, "push bad")                                                                        \
    X(POP, 0, "drop the top value")                                                                \
    X(DUP, 0, "push a copy of the top value")                                                      \
    /* Variables */                                                                                \
    X(GET_LOCAL, 1, "push local slot [a]")                                                         \
    X(SET_LOCAL, 1, "pop into local slot [a]")                                                     \
    X(BOX_LOCAL, 1, "put local [a] in a cell, for closures to share")                              \
    X(GET_CELL, 1, "push the value in local [a]'s cell")                                           \
    X(SET_CELL, 1, "pop into local [a]'s cell")                                                    \
    X(GET_CAPTURE, 1, "push captured variable [a] of the running closure")                         \
    X(SET_CAPTURE, 1, "pop into captured variable [a] of the running closure")                     \
    X(GET_SELF, 0, "push the running function itself (a nested function calling itself)")          \
    X(MAKE_CLOSURE, 1, "a closure of function constant [a], capturing what it lists")              \
    X(GET_GLOBAL, 1, "push global slot [a]")                                                       \
    X(SET_GLOBAL, 1, "pop into global slot [a]")                                                   \
    /* Integers: overflow and division by zero are runtime errors */                               \
    X(ADD_INT, 0, "a + b")                                                                         \
    X(SUB_INT, 0, "a - b")                                                                         \
    X(MUL_INT, 0, "a * b")                                                                         \
    X(FLOORDIV_INT, 0, "a // b")                                                                   \
    X(MOD_INT, 0, "a % b, with the sign of b")                                                     \
    X(POW_INT, 0, "a ** b, b >= 0")                                                                \
    X(NEG_INT, 0, "-a")                                                                            \
    X(BAND, 0, "a & b")                                                                            \
    X(BOR, 0, "a | b")                                                                             \
    X(BXOR, 0, "a ^ b")                                                                            \
    X(BNOT, 0, "~a")                                                                               \
    X(SHL, 0, "a << b, 0 <= b <= 63")                                                              \
    X(SHR, 0, "a >> b (arithmetic), 0 <= b <= 63")                                                 \
    X(LT_INT, 0, "a < b")                                                                          \
    X(LE_INT, 0, "a <= b")                                                                         \
    X(GT_INT, 0, "a > b")                                                                          \
    X(GE_INT, 0, "a >= b")                                                                         \
    X(EQ_INT, 0, "a == b")                                                                         \
    X(NE_INT, 0, "a != b")                                                                         \
    X(ADD_INT_K, 1, "a + int constant [a]")                                                        \
    X(SUB_INT_K, 1, "a - int constant [a]")                                                        \
    X(MUL_INT_K, 1, "a * int constant [a]")                                                        \
    X(FLOORDIV_INT_K, 1, "a // int constant [a], which is above 0")                                \
    X(MOD_INT_K, 1, "a % int constant [a], which is above 0")                                      \
    X(ADD_INT_L, 1, "a + int local [a]")                                                           \
    X(SUB_INT_L, 1, "a - int local [a]")                                                           \
    X(MUL_INT_L, 1, "a * int local [a]")                                                           \
    X(ADD_INT_LK, 2, "push int local [a] + int constant [b]")                                      \
    X(SUB_INT_LK, 2, "push int local [a] - int constant [b]")                                      \
    X(MUL_INT_LK, 2, "push int local [a] * int constant [b]")                                      \
    X(FLOORDIV_INT_LK, 2, "push int local [a] // int constant [b], which is above 0")              \
    X(MOD_INT_LK, 2, "push int local [a] % int constant [b], which is above 0")                    \
    X(LOCAL_ADD_INT_K, 2, "int local [a] += int constant [b]")                                     \
    X(LOCAL_SUB_INT_K, 2, "int local [a] -= int constant [b]")                                     \
    X(LOCAL_MUL_INT_K, 2, "int local [a] *= int constant [b]")                                     \
    X(LOCAL_FLOORDIV_INT_K, 2, "int local [a] //= int constant [b], which is above 0")             \
    X(LOCAL_MOD_INT_K, 2, "int local [a] %= int constant [b], which is above 0")                   \
    X(LOCAL_ADD_INT_L, 2, "int local [a] += int local [b]")                                        \
    X(LOCAL_SUB_INT_L, 2, "int local [a] -= int local [b]")                                        \
    X(GLOBAL_ADD_INT_K, 2, "int global [a] += int constant [b]")                                   \
    X(GLOBAL_SUB_INT_K, 2, "int global [a] -= int constant [b]")                                   \
    X(GLOBAL_ADD_INT_L, 2, "int global [a] += int local [b]")                                      \
    X(GLOBAL_SUB_INT_L, 2, "int global [a] -= int local [b]")                                      \
    /* Floats: IEEE 754 */                                                                         \
    X(ADD_FLOAT, 0, "a + b")                                                                       \
    X(SUB_FLOAT, 0, "a - b")                                                                       \
    X(MUL_FLOAT, 0, "a * b")                                                                       \
    X(DIV_FLOAT, 0, "a / b")                                                                       \
    X(FLOORDIV_FLOAT, 0, "floor(a / b)")                                                           \
    X(MOD_FLOAT, 0, "a % b, with the sign of b")                                                   \
    X(POW_FLOAT, 0, "a ** b")                                                                      \
    X(NEG_FLOAT, 0, "-a")                                                                          \
    X(LT_FLOAT, 0, "a < b")                                                                        \
    X(LE_FLOAT, 0, "a <= b")                                                                       \
    X(GT_FLOAT, 0, "a > b")                                                                        \
    X(GE_FLOAT, 0, "a >= b")                                                                       \
    X(EQ_FLOAT, 0, "a == b")                                                                       \
    X(NE_FLOAT, 0, "a != b")                                                                       \
    X(ADD_FLOAT_K, 1, "a + float constant [a]")                                                    \
    X(SUB_FLOAT_K, 1, "a - float constant [a]")                                                    \
    X(MUL_FLOAT_K, 1, "a * float constant [a]")                                                    \
    X(DIV_FLOAT_K, 1, "a / float constant [a]")                                                    \
    X(ADD_FLOAT_L, 1, "a + float local [a]")                                                       \
    X(SUB_FLOAT_L, 1, "a - float local [a]")                                                       \
    X(MUL_FLOAT_L, 1, "a * float local [a]")                                                       \
    X(DIV_FLOAT_L, 1, "a / float local [a]")                                                       \
    X(INT_TO_FLOAT, 0, "convert the top value")                                                    \
    /* Strings */                                                                                  \
    X(CONCAT, 0, "a + b for strings")                                                              \
    X(LT_STR, 0, "a < b by code point")                                                            \
    X(LE_STR, 0, "a <= b by code point")                                                           \
    X(GT_STR, 0, "a > b by code point")                                                            \
    X(GE_STR, 0, "a >= b by code point")                                                           \
    X(TO_STRING, 1, "text of the top value, with format constant [a] (0xFFFF: none)")              \
    X(BUILD_STRING, 1, "join the top [a] strings")                                                 \
    X(APPEND_LOCAL, 2, "str local [a] += the top [b] strings, in place if it has one owner")       \
    X(APPEND_GLOBAL, 2, "str global [a] += the top [b] strings, in place if it has one owner")     \
    /* Lists */                                                                                    \
    X(BUILD_MAP, 1, "a map of the top [a] key, value pairs")                                       \
    X(INDEX_STR, 0, "the character at str[index], or an index error")                              \
    X(INDEX_MAP, 0, "map[key], or a missing-key error")                                            \
    X(BUILD_LIST, 1, "a list of the top [a] values")                                               \
    X(INDEX_LIST, 0, "list[index], or an index error")                                             \
    X(SLICE, 1, "list or str [low..high] (..= if [a]) as a new one")                               \
    X(CONTAINS, 0, "item in collection")                                                           \
    X(CONCAT_LIST, 0, "a + b for lists")                                                           \
    X(METHOD, 2, "call method [a] with [b] arguments on the value under them")                     \
    X(METHOD_LOCAL, 3, "call method [b] with [c] arguments on local [a] in place")                 \
    X(METHOD_GLOBAL, 3, "call method [b] with [c] arguments on global [a] in place")               \
    X(METHOD_CAPTURE, 3, "call method [b] with [c] arguments on captured variable [a] in place")   \
    X(SET_INDEX_LOCAL, 1, "local [a][index] = value, in place")                                    \
    X(SET_INDEX_GLOBAL, 1, "global [a][index] = value, in place")                                  \
    X(SET_INDEX_CAPTURE, 1, "captured variable [a][index] = value, in place")                      \
    X(SET_PATH_LOCAL, 2, "local [a], [b] keys down: set the item, copying shared levels")          \
    X(SET_PATH_GLOBAL, 2, "global [a], [b] keys down: set the item, copying shared levels")        \
    X(SET_PATH_CAPTURE, 2, "captured variable [a], [b] keys down: set the item")                   \
    X(SET_PATH_FIELD, 2, "object.field [a], [b] keys down: set the item, copying shared levels")   \
    X(METHOD_PATH_FIELD, 3,                                                                        \
      "method [b] on the item [c / 256] keys into object.field [a], [c % 256] arguments")          \
    X(METHOD_PATH_LOCAL, 3,                                                                        \
      "method [b] on the item [c / 256] keys into local [a], [c % 256] arguments")                 \
    X(METHOD_PATH_GLOBAL, 3,                                                                       \
      "method [b] on the item [c / 256] keys into global [a], [c % 256] arguments")                \
    X(METHOD_PATH_CAPTURE, 3,                                                                      \
      "method [b] on the item [c / 256] keys into captured variable [a], [c % 256] arguments")     \
    X(FOR_EACH, 3,                                                                                 \
      "next item of local [a] (position in [a+1]) into local [b], or jump forward [c]")            \
    X(FOR_EACH2, 3, "next position and item of local [a] into locals [b], [b+1], or jump [c]")     \
    /* Trees */                                                                                    \
    X(NEW_OBJECT, 2, "an object of tree constant [a] from its [b] fields on the stack")            \
    X(GET_FIELD, 1, "field [a] of the object on top")                                              \
    X(SET_FIELD, 1, "object.field [a] = value (shared: every holder sees it)")                     \
    X(GET_FIELD_L, 2, "field [b] of the object in local [a]")                                      \
    X(SET_FIELD_L, 2, "pop into field [b] of the object in local [a]")                             \
    X(FIELD_ADD_INT_K, 3, "int field [b] of the object in local [a] += int constant [c]")          \
    X(FIELD_SUB_INT_K, 3, "int field [b] of the object in local [a] -= int constant [c]")          \
    X(FIELD_ADD_INT_L, 3, "int field [b] of the object in local [a] += int local [c]")             \
    X(FIELD_SUB_INT_L, 3, "int field [b] of the object in local [a] -= int local [c]")             \
    X(FIELD_ADD_FLOAT_K, 3, "float field [b] of the object in local [a] += float constant [c]")    \
    X(FIELD_SUB_FLOAT_K, 3, "float field [b] of the object in local [a] -= float constant [c]")    \
    X(FIELD_ADD_FLOAT_L, 3, "float field [b] of the object in local [a] += float local [c]")       \
    X(FIELD_SUB_FLOAT_L, 3, "float field [b] of the object in local [a] -= float local [c]")       \
    X(IS_TREE, 1, "is the top value an object of tree constant [a] or one extending it")           \
    X(WEAK, 0, "the top value as a weak reference (an object's weak box; none stays none)")        \
    X(UNWEAK, 0, "a weak reference's object, or none if it is gone")                               \
    X(IS_TRAIT, 1, "does the top value's tree have the trait named by constant [a]")               \
    X(INVOKE, 2, "call method [a] of the object under [b] arguments, the object as self")          \
    X(INVOKE_FROM, 3, "call tree constant [a]'s method [b] (super, init) with [c] arguments")      \
    X(CALL_METHOD, 2, "call program method [a], found when compiling, the object under [b] args")  \
    /* Every type */                                                                               \
    X(EQ, 0, "a == b, for any two values")                                                         \
    X(NE, 0, "a != b, for any two values")                                                         \
    X(NOT, 0, "not a")                                                                             \
    X(IS_NONE, 0, "a == none")                                                                     \
    /* Control flow: jumps are relative to the next instruction */                                 \
    X(JUMP, 1, "jump forward [a] bytes")                                                           \
    X(JUMP_IF_FALSE, 1, "pop; jump forward [a] bytes if bad")                                      \
    X(JUMP_IF_FALSE_KEEP, 1, "jump forward [a] bytes if bad, keeping the value")                   \
    X(JUMP_IF_TRUE_KEEP, 1, "jump forward [a] bytes if good, keeping the value")                   \
    X(JUMP_IF_NOT_NONE_KEEP, 1, "jump forward [a] bytes unless none, keeping the value")           \
    X(JUMP_IF_NONE_KEEP, 1, "jump forward [a] bytes if none, keeping the value")                   \
    X(JUMP_IF_NOT_LT_INT, 1, "pop two ints; jump forward [a] bytes unless a < b")                  \
    X(JUMP_IF_NOT_LE_INT, 1, "pop two ints; jump forward [a] bytes unless a <= b")                 \
    X(JUMP_IF_NOT_GT_INT, 1, "pop two ints; jump forward [a] bytes unless a > b")                  \
    X(JUMP_IF_NOT_GE_INT, 1, "pop two ints; jump forward [a] bytes unless a >= b")                 \
    X(JUMP_IF_NOT_EQ_INT, 1, "pop two ints; jump forward [a] bytes unless a == b")                 \
    X(JUMP_IF_NOT_NE_INT, 1, "pop two ints; jump forward [a] bytes unless a != b")                 \
    X(JUMP_IF_NOT_LT_FLOAT, 1, "pop two floats; jump forward [a] bytes unless a < b")              \
    X(JUMP_IF_NOT_LE_FLOAT, 1, "pop two floats; jump forward [a] bytes unless a <= b")             \
    X(JUMP_IF_NOT_GT_FLOAT, 1, "pop two floats; jump forward [a] bytes unless a > b")              \
    X(JUMP_IF_NOT_GE_FLOAT, 1, "pop two floats; jump forward [a] bytes unless a >= b")             \
    X(JUMP_IF_NOT_EQ_FLOAT, 1, "pop two floats; jump forward [a] bytes unless a == b")             \
    X(JUMP_IF_NOT_NE_FLOAT, 1, "pop two floats; jump forward [a] bytes unless a != b")             \
    X(JUMP_IF_NOT_LT_INT_K, 2, "pop a (int); jump forward [b] bytes unless a < constant [a]")      \
    X(JUMP_IF_NOT_LE_INT_K, 2, "pop a (int); jump forward [b] bytes unless a <= constant [a]")     \
    X(JUMP_IF_NOT_GT_INT_K, 2, "pop a (int); jump forward [b] bytes unless a > constant [a]")      \
    X(JUMP_IF_NOT_GE_INT_K, 2, "pop a (int); jump forward [b] bytes unless a >= constant [a]")     \
    X(JUMP_IF_NOT_EQ_INT_K, 2, "pop a (int); jump forward [b] bytes unless a == constant [a]")     \
    X(JUMP_IF_NOT_NE_INT_K, 2, "pop a (int); jump forward [b] bytes unless a != constant [a]")     \
    X(JUMP_IF_NOT_LT_FLOAT_K, 2, "pop a (float); jump forward [b] bytes unless a < constant [a]")  \
    X(JUMP_IF_NOT_LE_FLOAT_K, 2, "pop a (float); jump forward [b] bytes unless a <= constant [a]") \
    X(JUMP_IF_NOT_GT_FLOAT_K, 2, "pop a (float); jump forward [b] bytes unless a > constant [a]")  \
    X(JUMP_IF_NOT_GE_FLOAT_K, 2, "pop a (float); jump forward [b] bytes unless a >= constant [a]") \
    X(JUMP_IF_NOT_EQ_FLOAT_K, 2, "pop a (float); jump forward [b] bytes unless a == constant [a]") \
    X(JUMP_IF_NOT_NE_FLOAT_K, 2, "pop a (float); jump forward [b] bytes unless a != constant [a]") \
    X(JUMP_IF_NOT_LT_INT_LK, 3, "jump [c] unless int local [a] < int constant [b]")                \
    X(JUMP_IF_NOT_LE_INT_LK, 3, "jump [c] unless int local [a] <= int constant [b]")               \
    X(JUMP_IF_NOT_GT_INT_LK, 3, "jump [c] unless int local [a] > int constant [b]")                \
    X(JUMP_IF_NOT_GE_INT_LK, 3, "jump [c] unless int local [a] >= int constant [b]")               \
    X(JUMP_IF_NOT_EQ_INT_LK, 3, "jump [c] unless int local [a] == int constant [b]")               \
    X(JUMP_IF_NOT_NE_INT_LK, 3, "jump [c] unless int local [a] != int constant [b]")               \
    X(LOOP, 1, "jump back [a] bytes")                                                              \
    X(FOR_RANGE, 3, "jump forward [c] if local [a] is past local [a+1] (<, or <= if [b])")         \
    X(FOR_NEXT, 3,                                                                                 \
      "if local [a] + 1 is within local [a+1] (<, or <= if [b]): add 1, jump back [c]")            \
    /* Errors */                                                                                   \
    X(TRY, 1, "until END_TRY: on an error, unwind and jump forward [a] with the error pushed")     \
    X(END_TRY, 1, "the last [a] tries are over")                                                   \
    X(THROW, 0, "throw the error on top")                                                          \
    /* Calls */                                                                                    \
    X(CALL, 1, "call the function under [a] arguments")                                            \
    X(CALL_GLOBAL, 2, "call the function in global [a] with the [b] arguments on top")             \
    X(CALL_SELF, 1, "call the running function with the [a] arguments on top")                     \
    X(TAIL_SELF, 1, "return what the running function returns for the [a] arguments on top")       \
    X(CALL_BUILTIN, 2, "call built-in [a] with [b] arguments")                                     \
    X(SPAWN, 1, "a task running the function under [a] arguments; push the task")                  \
    X(CALL_NATIVE, 2, "call standard-library function [a] with [b] arguments")                     \
    X(RETURN, 0, "return the top value")                                                           \
    X(RETURN_LOCAL, 1, "return local slot [a]")                                                    \
    X(RETURN_ADD_INT, 0, "return a + b")                                                           \
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
    X(SET, "set")         \
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
