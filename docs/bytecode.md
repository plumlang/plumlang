# Plumlang bytecode

The compiler (`src/compiler/`) turns a checked program into bytecode, which
the virtual machine (`src/vm/`) runs. `plum dis file.plum` prints it.

## Functions and chunks

Every function, and the top level of the program (`<main>`), compiles to a
**chunk**:

- **code**: a sequence of instructions, each a one-byte opcode followed by
  its operands, each a little-endian 16-bit number;
- **constants**: the numbers, strings and functions the code uses, each
  stored once;
- **lines**: the source line of every instruction, stored as runs, for
  error messages and stack traces.

A function also records its parameter count and its **slot count**: the
number of local variables it needs at once. Parameters take the first
slots; a block's variables take the next free ones and give them back when
the block ends.

## Variables

| Where it is declared | Where it lives | Instructions |
|---|---|---|
| Module level (`plant`, `root`, `branch`) | a global slot, numbered in declaration order | `GET_GLOBAL`, `SET_GLOBAL` |
| Inside a function, or in a block at the top level | a local slot of the running function | `GET_LOCAL`, `SET_LOCAL` |

Module functions are put in their global slots before the first statement
runs, which is how a function can be called before its declaration.

## The stack

Instructions work on a stack of values. `ADD_INT` pops two ints and pushes
their sum; `CALL n` finds the function under its `n` arguments, runs it, and
leaves its result in their place. Every value on the stack, in a slot or in
a constant pool owns one reference to its object (spec 11.2).

## Types decide the instruction

The type checker has proven every operand's type, so the compiler picks a
typed instruction: `ADD_INT` or `ADD_FLOAT`, `LT_INT`, `LT_FLOAT` or
`LT_STR`. The one implicit conversion, int to float, is a separate
`INT_TO_FLOAT` wherever the checker allowed it. The VM never looks at a
value's type to decide what to do.

## Range loops

`for i in a..b` uses no range object: `i` and a hidden end value live in
neighbouring slots. `FOR_RANGE` at the top leaves the loop once `i` passes
the end; `FOR_NEXT` after the body adds one and jumps back. For `a..=b`,
`FOR_NEXT` stops when `i` equals the end before adding, so a range ending
at the largest int cannot overflow.

## Instructions

Jump distances are relative to the instruction after the jump. Generated
from `src/compiler/opcodes.h` by `make bytecode-doc`; `make test` checks it.

<!-- opcodes:begin -->
| # | Instruction | Operands | What it does |
|---|---|---|---|
| 0 | `CONST` | 1 | push constant [a] |
| 1 | `NONE` | 0 | push none |
| 2 | `TRUE` | 0 | push good |
| 3 | `FALSE` | 0 | push bad |
| 4 | `POP` | 0 | drop the top value |
| 5 | `DUP` | 0 | push a copy of the top value |
| 6 | `GET_LOCAL` | 1 | push local slot [a] |
| 7 | `SET_LOCAL` | 1 | pop into local slot [a] |
| 8 | `BOX_LOCAL` | 1 | put local [a] in a cell, for closures to share |
| 9 | `GET_CELL` | 1 | push the value in local [a]'s cell |
| 10 | `SET_CELL` | 1 | pop into local [a]'s cell |
| 11 | `GET_CAPTURE` | 1 | push captured variable [a] of the running closure |
| 12 | `SET_CAPTURE` | 1 | pop into captured variable [a] of the running closure |
| 13 | `GET_SELF` | 0 | push the running function itself (a nested function calling itself) |
| 14 | `MAKE_CLOSURE` | 1 | a closure of function constant [a], capturing what it lists |
| 15 | `GET_GLOBAL` | 1 | push global slot [a] |
| 16 | `SET_GLOBAL` | 1 | pop into global slot [a] |
| 17 | `ADD_INT` | 0 | a + b |
| 18 | `SUB_INT` | 0 | a - b |
| 19 | `MUL_INT` | 0 | a * b |
| 20 | `FLOORDIV_INT` | 0 | a // b |
| 21 | `MOD_INT` | 0 | a % b, with the sign of b |
| 22 | `POW_INT` | 0 | a ** b, b >= 0 |
| 23 | `NEG_INT` | 0 | -a |
| 24 | `BAND` | 0 | a & b |
| 25 | `BOR` | 0 | a  |
| 26 | `BXOR` | 0 | a ^ b |
| 27 | `BNOT` | 0 | ~a |
| 28 | `SHL` | 0 | a << b, 0 <= b <= 63 |
| 29 | `SHR` | 0 | a >> b (arithmetic), 0 <= b <= 63 |
| 30 | `LT_INT` | 0 | a < b |
| 31 | `LE_INT` | 0 | a <= b |
| 32 | `GT_INT` | 0 | a > b |
| 33 | `GE_INT` | 0 | a >= b |
| 34 | `EQ_INT` | 0 | a == b |
| 35 | `NE_INT` | 0 | a != b |
| 36 | `ADD_INT_K` | 1 | a + int constant [a] |
| 37 | `SUB_INT_K` | 1 | a - int constant [a] |
| 38 | `MUL_INT_K` | 1 | a * int constant [a] |
| 39 | `FLOORDIV_INT_K` | 1 | a // int constant [a], which is above 0 |
| 40 | `MOD_INT_K` | 1 | a % int constant [a], which is above 0 |
| 41 | `ADD_INT_L` | 1 | a + int local [a] |
| 42 | `SUB_INT_L` | 1 | a - int local [a] |
| 43 | `MUL_INT_L` | 1 | a * int local [a] |
| 44 | `LOCAL_ADD_INT_K` | 2 | int local [a] += int constant [b] |
| 45 | `LOCAL_SUB_INT_K` | 2 | int local [a] -= int constant [b] |
| 46 | `LOCAL_MUL_INT_K` | 2 | int local [a] *= int constant [b] |
| 47 | `LOCAL_FLOORDIV_INT_K` | 2 | int local [a] //= int constant [b], which is above 0 |
| 48 | `LOCAL_MOD_INT_K` | 2 | int local [a] %= int constant [b], which is above 0 |
| 49 | `LOCAL_ADD_INT_L` | 2 | int local [a] += int local [b] |
| 50 | `LOCAL_SUB_INT_L` | 2 | int local [a] -= int local [b] |
| 51 | `ADD_FLOAT` | 0 | a + b |
| 52 | `SUB_FLOAT` | 0 | a - b |
| 53 | `MUL_FLOAT` | 0 | a * b |
| 54 | `DIV_FLOAT` | 0 | a / b |
| 55 | `FLOORDIV_FLOAT` | 0 | floor(a / b) |
| 56 | `MOD_FLOAT` | 0 | a % b, with the sign of b |
| 57 | `POW_FLOAT` | 0 | a ** b |
| 58 | `NEG_FLOAT` | 0 | -a |
| 59 | `LT_FLOAT` | 0 | a < b |
| 60 | `LE_FLOAT` | 0 | a <= b |
| 61 | `GT_FLOAT` | 0 | a > b |
| 62 | `GE_FLOAT` | 0 | a >= b |
| 63 | `EQ_FLOAT` | 0 | a == b |
| 64 | `NE_FLOAT` | 0 | a != b |
| 65 | `ADD_FLOAT_K` | 1 | a + float constant [a] |
| 66 | `SUB_FLOAT_K` | 1 | a - float constant [a] |
| 67 | `MUL_FLOAT_K` | 1 | a * float constant [a] |
| 68 | `DIV_FLOAT_K` | 1 | a / float constant [a] |
| 69 | `ADD_FLOAT_L` | 1 | a + float local [a] |
| 70 | `SUB_FLOAT_L` | 1 | a - float local [a] |
| 71 | `MUL_FLOAT_L` | 1 | a * float local [a] |
| 72 | `DIV_FLOAT_L` | 1 | a / float local [a] |
| 73 | `INT_TO_FLOAT` | 0 | convert the top value |
| 74 | `CONCAT` | 0 | a + b for strings |
| 75 | `LT_STR` | 0 | a < b by code point |
| 76 | `LE_STR` | 0 | a <= b by code point |
| 77 | `GT_STR` | 0 | a > b by code point |
| 78 | `GE_STR` | 0 | a >= b by code point |
| 79 | `TO_STRING` | 1 | text of the top value, with format constant [a] (0xFFFF: none) |
| 80 | `BUILD_STRING` | 1 | join the top [a] strings |
| 81 | `BUILD_MAP` | 1 | a map of the top [a] key, value pairs |
| 82 | `INDEX_STR` | 0 | the character at str[index], or an index error |
| 83 | `INDEX_MAP` | 0 | map[key], or a missing-key error |
| 84 | `BUILD_LIST` | 1 | a list of the top [a] values |
| 85 | `INDEX_LIST` | 0 | list[index], or an index error |
| 86 | `SLICE` | 1 | list or str [low..high] (..= if [a]) as a new one |
| 87 | `CONTAINS` | 0 | item in collection |
| 88 | `CONCAT_LIST` | 0 | a + b for lists |
| 89 | `METHOD` | 2 | call method [a] with [b] arguments on the value under them |
| 90 | `METHOD_LOCAL` | 3 | call method [b] with [c] arguments on local [a] in place |
| 91 | `METHOD_GLOBAL` | 3 | call method [b] with [c] arguments on global [a] in place |
| 92 | `METHOD_CAPTURE` | 3 | call method [b] with [c] arguments on captured variable [a] in place |
| 93 | `SET_INDEX_LOCAL` | 1 | local [a][index] = value, in place |
| 94 | `SET_INDEX_GLOBAL` | 1 | global [a][index] = value, in place |
| 95 | `SET_INDEX_CAPTURE` | 1 | captured variable [a][index] = value, in place |
| 96 | `SET_PATH_LOCAL` | 2 | local [a], [b] keys down: set the item, copying shared levels |
| 97 | `SET_PATH_GLOBAL` | 2 | global [a], [b] keys down: set the item, copying shared levels |
| 98 | `SET_PATH_CAPTURE` | 2 | captured variable [a], [b] keys down: set the item |
| 99 | `SET_PATH_FIELD` | 2 | object.field [a], [b] keys down: set the item, copying shared levels |
| 100 | `FOR_EACH2` | 3 | next position and item of local [a] into locals [b], [b+1], or jump [c] |
| 101 | `NEW_OBJECT` | 2 | an object of tree constant [a] from its [b] fields on the stack |
| 102 | `GET_FIELD` | 1 | field [a] of the object on top |
| 103 | `SET_FIELD` | 1 | object.field [a] = value (shared: every holder sees it) |
| 104 | `IS_TREE` | 1 | is the top value an object of tree constant [a] or one extending it |
| 105 | `WEAK` | 0 | the top value as a weak reference (an object's weak box; none stays none) |
| 106 | `UNWEAK` | 0 | a weak reference's object, or none if it is gone |
| 107 | `IS_TRAIT` | 1 | does the top value's tree have the trait named by constant [a] |
| 108 | `INVOKE` | 2 | call method [a] of the object under [b] arguments, the object as self |
| 109 | `INVOKE_FROM` | 3 | call tree constant [a]'s method [b] (super, init) with [c] arguments |
| 110 | `EQ` | 0 | a == b, for any two values |
| 111 | `NE` | 0 | a != b, for any two values |
| 112 | `NOT` | 0 | not a |
| 113 | `IS_NONE` | 0 | a == none |
| 114 | `JUMP` | 1 | jump forward [a] bytes |
| 115 | `JUMP_IF_FALSE` | 1 | pop; jump forward [a] bytes if bad |
| 116 | `JUMP_IF_FALSE_KEEP` | 1 | jump forward [a] bytes if bad, keeping the value |
| 117 | `JUMP_IF_TRUE_KEEP` | 1 | jump forward [a] bytes if good, keeping the value |
| 118 | `JUMP_IF_NOT_NONE_KEEP` | 1 | jump forward [a] bytes unless none, keeping the value |
| 119 | `JUMP_IF_NONE_KEEP` | 1 | jump forward [a] bytes if none, keeping the value |
| 120 | `JUMP_IF_NOT_LT_INT` | 1 | pop two ints; jump forward [a] bytes unless a < b |
| 121 | `JUMP_IF_NOT_LE_INT` | 1 | pop two ints; jump forward [a] bytes unless a <= b |
| 122 | `JUMP_IF_NOT_GT_INT` | 1 | pop two ints; jump forward [a] bytes unless a > b |
| 123 | `JUMP_IF_NOT_GE_INT` | 1 | pop two ints; jump forward [a] bytes unless a >= b |
| 124 | `JUMP_IF_NOT_EQ_INT` | 1 | pop two ints; jump forward [a] bytes unless a == b |
| 125 | `JUMP_IF_NOT_NE_INT` | 1 | pop two ints; jump forward [a] bytes unless a != b |
| 126 | `JUMP_IF_NOT_LT_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a < b |
| 127 | `JUMP_IF_NOT_LE_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a <= b |
| 128 | `JUMP_IF_NOT_GT_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a > b |
| 129 | `JUMP_IF_NOT_GE_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a >= b |
| 130 | `JUMP_IF_NOT_EQ_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a == b |
| 131 | `JUMP_IF_NOT_NE_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a != b |
| 132 | `JUMP_IF_NOT_LT_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a < constant [a] |
| 133 | `JUMP_IF_NOT_LE_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a <= constant [a] |
| 134 | `JUMP_IF_NOT_GT_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a > constant [a] |
| 135 | `JUMP_IF_NOT_GE_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a >= constant [a] |
| 136 | `JUMP_IF_NOT_EQ_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a == constant [a] |
| 137 | `JUMP_IF_NOT_NE_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a != constant [a] |
| 138 | `JUMP_IF_NOT_LT_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a < constant [a] |
| 139 | `JUMP_IF_NOT_LE_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a <= constant [a] |
| 140 | `JUMP_IF_NOT_GT_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a > constant [a] |
| 141 | `JUMP_IF_NOT_GE_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a >= constant [a] |
| 142 | `JUMP_IF_NOT_EQ_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a == constant [a] |
| 143 | `JUMP_IF_NOT_NE_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a != constant [a] |
| 144 | `LOOP` | 1 | jump back [a] bytes |
| 145 | `FOR_RANGE` | 3 | jump forward [c] if local [a] is past local [a+1] (<, or <= if [b]) |
| 146 | `TRY` | 1 | until END_TRY: on an error, unwind and jump forward [a] with the error pushed |
| 147 | `END_TRY` | 1 | the last [a] tries are over |
| 148 | `THROW` | 0 | throw the error on top |
| 149 | `CALL` | 1 | call the function under [a] arguments |
| 150 | `CALL_BUILTIN` | 2 | call built-in [a] with [b] arguments |
| 151 | `SPAWN` | 1 | a task running the function under [a] arguments; push the task |
| 152 | `CALL_NATIVE` | 2 | call standard-library function [a] with [b] arguments |
| 153 | `RETURN` | 0 | return the top value |
| 154 | `RETURN_NONE` | 0 | return none |
<!-- opcodes:end -->
