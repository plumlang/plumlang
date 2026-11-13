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
| 8 | `GET_GLOBAL` | 1 | push global slot [a] |
| 9 | `SET_GLOBAL` | 1 | pop into global slot [a] |
| 10 | `ADD_INT` | 0 | a + b |
| 11 | `SUB_INT` | 0 | a - b |
| 12 | `MUL_INT` | 0 | a * b |
| 13 | `FLOORDIV_INT` | 0 | a // b |
| 14 | `MOD_INT` | 0 | a % b, with the sign of b |
| 15 | `POW_INT` | 0 | a ** b, b >= 0 |
| 16 | `NEG_INT` | 0 | -a |
| 17 | `BAND` | 0 | a & b |
| 18 | `BOR` | 0 | a  |
| 19 | `BXOR` | 0 | a ^ b |
| 20 | `BNOT` | 0 | ~a |
| 21 | `SHL` | 0 | a << b, 0 <= b <= 63 |
| 22 | `SHR` | 0 | a >> b (arithmetic), 0 <= b <= 63 |
| 23 | `LT_INT` | 0 | a < b |
| 24 | `LE_INT` | 0 | a <= b |
| 25 | `GT_INT` | 0 | a > b |
| 26 | `GE_INT` | 0 | a >= b |
| 27 | `EQ_INT` | 0 | a == b |
| 28 | `NE_INT` | 0 | a != b |
| 29 | `ADD_FLOAT` | 0 | a + b |
| 30 | `SUB_FLOAT` | 0 | a - b |
| 31 | `MUL_FLOAT` | 0 | a * b |
| 32 | `DIV_FLOAT` | 0 | a / b |
| 33 | `FLOORDIV_FLOAT` | 0 | floor(a / b) |
| 34 | `MOD_FLOAT` | 0 | a % b, with the sign of b |
| 35 | `POW_FLOAT` | 0 | a ** b |
| 36 | `NEG_FLOAT` | 0 | -a |
| 37 | `LT_FLOAT` | 0 | a < b |
| 38 | `LE_FLOAT` | 0 | a <= b |
| 39 | `GT_FLOAT` | 0 | a > b |
| 40 | `GE_FLOAT` | 0 | a >= b |
| 41 | `EQ_FLOAT` | 0 | a == b |
| 42 | `NE_FLOAT` | 0 | a != b |
| 43 | `INT_TO_FLOAT` | 0 | convert the top value |
| 44 | `CONCAT` | 0 | a + b for strings |
| 45 | `LT_STR` | 0 | a < b by code point |
| 46 | `LE_STR` | 0 | a <= b by code point |
| 47 | `GT_STR` | 0 | a > b by code point |
| 48 | `GE_STR` | 0 | a >= b by code point |
| 49 | `TO_STRING` | 1 | text of the top value, with format constant [a] (0xFFFF: none) |
| 50 | `BUILD_STRING` | 1 | join the top [a] strings |
| 51 | `BUILD_LIST` | 1 | a list of the top [a] values |
| 52 | `INDEX_LIST` | 0 | list[index], or an index error |
| 53 | `SLICE` | 1 | list[low..high] (..= if [a]) as a new list |
| 54 | `CONTAINS` | 0 | item in collection |
| 55 | `CONCAT_LIST` | 0 | a + b for lists |
| 56 | `METHOD` | 2 | call method [a] with [b] arguments on the value under them |
| 57 | `METHOD_LOCAL` | 3 | call method [b] with [c] arguments on local [a] in place |
| 58 | `METHOD_GLOBAL` | 3 | call method [b] with [c] arguments on global [a] in place |
| 59 | `SET_INDEX_LOCAL` | 1 | local [a][index] = value, in place |
| 60 | `SET_INDEX_GLOBAL` | 1 | global [a][index] = value, in place |
| 61 | `FOR_EACH2` | 3 | next position and item of local [a] into locals [b], [b+1], or jump [c] |
| 62 | `EQ` | 0 | a == b, for any two values |
| 63 | `NE` | 0 | a != b, for any two values |
| 64 | `NOT` | 0 | not a |
| 65 | `IS_NONE` | 0 | a == none |
| 66 | `JUMP` | 1 | jump forward [a] bytes |
| 67 | `JUMP_IF_FALSE` | 1 | pop; jump forward [a] bytes if bad |
| 68 | `JUMP_IF_FALSE_KEEP` | 1 | jump forward [a] bytes if bad, keeping the value |
| 69 | `JUMP_IF_TRUE_KEEP` | 1 | jump forward [a] bytes if good, keeping the value |
| 70 | `JUMP_IF_NOT_NONE_KEEP` | 1 | jump forward [a] bytes unless none, keeping the value |
| 71 | `LOOP` | 1 | jump back [a] bytes |
| 72 | `FOR_RANGE` | 3 | jump forward [c] if local [a] is past local [a+1] (<, or <= if [b]) |
| 73 | `FOR_NEXT` | 3 | unless [b] and local [a] == local [a+1]: add 1, jump back [c] |
| 74 | `CALL` | 1 | call the function under [a] arguments |
| 75 | `CALL_BUILTIN` | 2 | call built-in [a] with [b] arguments |
| 76 | `RETURN` | 0 | return the top value |
| 77 | `RETURN_NONE` | 0 | return none |
<!-- opcodes:end -->
