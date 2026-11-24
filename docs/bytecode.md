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

Where an operand is a literal or a variable, it is often an operand of the
instruction rather than a value on the stack, which saves instructions in
hot loops. Each form exists only for ints or floats, which the checker has
proven, and gives the same results and the same errors, at the same line,
as the plain instruction:

| Form | Example | Instead of |
|---|---|---|
| `_K`: the right operand is a constant | `n % 2`: `MOD_INT_K` | `CONST`, `MOD_INT` |
| `_L`: the right operand is a local | `x * y`: `MUL_FLOAT_L` | `GET_LOCAL`, `MUL_FLOAT` |
| `_LK`: a local and a literal from -32768 to 32767, held by the instruction itself, pushed | `i + 1`: `ADD_INT_LK` | `GET_LOCAL`, `CONST`, `ADD_INT` |
| `LOCAL_`, `GLOBAL_`: change a variable in place | `n += 1`, `n = n // 2`: `LOCAL_ADD_INT_K`, `LOCAL_FLOORDIV_INT_K` | `GET_LOCAL`, `CONST`, `ADD_INT`, `SET_LOCAL` |
| `JUMP_IF_NOT_`: compare and jump, for `if` and `while` | `while i < n`: `JUMP_IF_NOT_LT_INT` | `LT_INT`, `JUMP_IF_FALSE` |

The int forms that take a constant for `//` and `%` are used only when the
constant is above 0, so they never divide by zero or overflow. An int
literal in float arithmetic becomes a float constant at compile time, as
`INT_TO_FLOAT` would make it at run time. A condition `a and b` compiles
to one jump out per operand, with no `good` or `bad` left to pop. The
local forms read the running function's own slots, so a variable a
closure captured (which lives in a cell) uses the plain instructions.

## Building text

An interpolated string pushes the text of each part and joins them with
one `BUILD_STRING`; so does a chain of `+` on strings (`a + b + "{c}"`),
rather than making a new string for every `+`.

`s += text` and `s = s + text`, on a str local or module variable, push
the pieces of `text` the same way and then `APPEND_LOCAL` or
`APPEND_GLOBAL`, which adds them to `s` itself. When `s` is the string's
only owner (its count is 1) and the string isn't frozen, its buffer grows
in place, by half again each time it is full, so a loop adding a line at
a time is linear rather than quadratic. Otherwise `s` gets a new string
(copy-on-write), so another holder of the old one never sees a change.
The compiler freezes its constant strings. A module variable uses
`APPEND_GLOBAL` only when computing `text` makes no call, which could
change the variable before it is read; a captured variable (in a cell)
and a field use the plain instructions.

## Range loops

`for i in a..b` uses no range object: `i` and a hidden end value live in
neighbouring slots. `FOR_RANGE` before the body skips the loop when `i` is
already past the end; `FOR_NEXT` after the body adds one and jumps back to
the body while the new `i` is still in the range, so each time round runs
one loop instruction. It checks before adding, so a range ending at the
largest int (`a..=b`) cannot overflow.

## Fields and method calls

An object stores its fields in one array, the parent tree's fields first,
so a field's position is fixed for its tree and every tree extending it:
`obj.x` is `GET_FIELD` with that position, never a lookup by name. When
the object is in a local slot of the running function (`self`, a
parameter, a variable no closure captured), the instruction names the
slot too, and nothing is pushed or counted for the object:

| Form | Example | Instead of |
|---|---|---|
| `GET_FIELD_L` | `self.r` | `GET_LOCAL`, `GET_FIELD` |
| `SET_FIELD_L` | `self.w = side` | `GET_LOCAL`, value, `SET_FIELD` |
| `FIELD_ADD_INT_K`, `FIELD_SUB_FLOAT_L`, ... | `self.count += 1`, `self.total -= x` | `GET_LOCAL`, `GET_LOCAL`, `GET_FIELD`, `ADD_INT_K`, `SET_FIELD` |

The in-place forms exist for `+=` and `-=` on an int or float field, with
an int or float literal or local on the right; they give the same results
and errors, at the same line, as the plain instructions. Anything else
(`?.`, a weak field, an object that isn't in a plain local) uses
`GET_FIELD` and `SET_FIELD`.

Every method name has a number, and every tree a table from those numbers
to the functions its objects run: its own methods, then its parent's,
then its traits' defaults. `INVOKE` takes the method from the table of
the object's own tree, so an override or a trait's implementation is
found with one array index, whatever the variable's type. When the
compiler knows which function will run, the call names it directly:
`CALL_METHOD n` calls the program's method function `n`, with no table.
That is the case when the receiver's type is a tree and no tree in the
whole program that extends it overrides the method, and for `super` and
`init` calls. A trait's type, an overridden method and code typed in the
REPL (a later entry may add a tree that overrides) keep `INVOKE`.

A method's frame has the object as slot 0 (`self`) and its arguments in
the next slots, where the caller pushed them; nothing sits under the
object (a plain function call has the function there), and the result
takes the object's place. Bytecode is never changed while it runs: there
are no inline caches to write, so isolates can share it (#150).

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
| 44 | `ADD_INT_LK` | 2 | push int local [a] + [b] |
| 45 | `SUB_INT_LK` | 2 | push int local [a] - [b] |
| 46 | `MUL_INT_LK` | 2 | push int local [a] * [b] |
| 47 | `FLOORDIV_INT_LK` | 2 | push int local [a] // [b], which is above 0 |
| 48 | `MOD_INT_LK` | 2 | push int local [a] % [b], which is above 0 |
| 49 | `LOCAL_ADD_INT_K` | 2 | int local [a] += int constant [b] |
| 50 | `LOCAL_SUB_INT_K` | 2 | int local [a] -= int constant [b] |
| 51 | `LOCAL_MUL_INT_K` | 2 | int local [a] *= int constant [b] |
| 52 | `LOCAL_FLOORDIV_INT_K` | 2 | int local [a] //= int constant [b], which is above 0 |
| 53 | `LOCAL_MOD_INT_K` | 2 | int local [a] %= int constant [b], which is above 0 |
| 54 | `LOCAL_ADD_INT_L` | 2 | int local [a] += int local [b] |
| 55 | `LOCAL_SUB_INT_L` | 2 | int local [a] -= int local [b] |
| 56 | `GLOBAL_ADD_INT_K` | 2 | int global [a] += int constant [b] |
| 57 | `GLOBAL_SUB_INT_K` | 2 | int global [a] -= int constant [b] |
| 58 | `GLOBAL_ADD_INT_L` | 2 | int global [a] += int local [b] |
| 59 | `GLOBAL_SUB_INT_L` | 2 | int global [a] -= int local [b] |
| 60 | `ADD_FLOAT` | 0 | a + b |
| 61 | `SUB_FLOAT` | 0 | a - b |
| 62 | `MUL_FLOAT` | 0 | a * b |
| 63 | `DIV_FLOAT` | 0 | a / b |
| 64 | `FLOORDIV_FLOAT` | 0 | floor(a / b) |
| 65 | `MOD_FLOAT` | 0 | a % b, with the sign of b |
| 66 | `POW_FLOAT` | 0 | a ** b |
| 67 | `NEG_FLOAT` | 0 | -a |
| 68 | `LT_FLOAT` | 0 | a < b |
| 69 | `LE_FLOAT` | 0 | a <= b |
| 70 | `GT_FLOAT` | 0 | a > b |
| 71 | `GE_FLOAT` | 0 | a >= b |
| 72 | `EQ_FLOAT` | 0 | a == b |
| 73 | `NE_FLOAT` | 0 | a != b |
| 74 | `ADD_FLOAT_K` | 1 | a + float constant [a] |
| 75 | `SUB_FLOAT_K` | 1 | a - float constant [a] |
| 76 | `MUL_FLOAT_K` | 1 | a * float constant [a] |
| 77 | `DIV_FLOAT_K` | 1 | a / float constant [a] |
| 78 | `ADD_FLOAT_L` | 1 | a + float local [a] |
| 79 | `SUB_FLOAT_L` | 1 | a - float local [a] |
| 80 | `MUL_FLOAT_L` | 1 | a * float local [a] |
| 81 | `DIV_FLOAT_L` | 1 | a / float local [a] |
| 82 | `INT_TO_FLOAT` | 0 | convert the top value |
| 83 | `CONCAT` | 0 | a + b for strings |
| 84 | `LT_STR` | 0 | a < b by code point |
| 85 | `LE_STR` | 0 | a <= b by code point |
| 86 | `GT_STR` | 0 | a > b by code point |
| 87 | `GE_STR` | 0 | a >= b by code point |
| 88 | `TO_STRING` | 1 | text of the top value, with format constant [a] (0xFFFF: none) |
| 89 | `BUILD_STRING` | 1 | join the top [a] strings |
| 90 | `APPEND_LOCAL` | 2 | str local [a] += the top [b] strings, in place if it has one owner |
| 91 | `APPEND_GLOBAL` | 2 | str global [a] += the top [b] strings, in place if it has one owner |
| 92 | `BUILD_MAP` | 1 | a map of the top [a] key, value pairs |
| 93 | `INDEX_STR` | 0 | the character at str[index], or an index error |
| 94 | `INDEX_MAP` | 0 | map[key], or a missing-key error |
| 95 | `BUILD_LIST` | 1 | a list of the top [a] values |
| 96 | `INDEX_LIST` | 0 | list[index], or an index error |
| 97 | `INDEX_LIST_L` | 1 | local [a][index] for a list in a local, which isn't pushed or counted |
| 98 | `LEN_L` | 1 | len() of the str, list, map or set in local [a], which isn't pushed or counted |
| 99 | `SLICE` | 1 | list or str [low..high] (..= if [a]) as a new one |
| 100 | `CONTAINS` | 0 | item in collection |
| 101 | `CONCAT_LIST` | 0 | a + b for lists |
| 102 | `METHOD` | 2 | call method [a] with [b] arguments on the value under them |
| 103 | `METHOD_LOCAL` | 3 | call method [b] with [c] arguments on local [a] in place |
| 104 | `METHOD_GLOBAL` | 3 | call method [b] with [c] arguments on global [a] in place |
| 105 | `METHOD_CAPTURE` | 3 | call method [b] with [c] arguments on captured variable [a] in place |
| 106 | `SET_INDEX_LOCAL` | 1 | local [a][index] = value, in place |
| 107 | `SET_INDEX_GLOBAL` | 1 | global [a][index] = value, in place |
| 108 | `SET_INDEX_CAPTURE` | 1 | captured variable [a][index] = value, in place |
| 109 | `SET_PATH_LOCAL` | 2 | local [a], [b] keys down: set the item, copying shared levels |
| 110 | `SET_PATH_GLOBAL` | 2 | global [a], [b] keys down: set the item, copying shared levels |
| 111 | `SET_PATH_CAPTURE` | 2 | captured variable [a], [b] keys down: set the item |
| 112 | `SET_PATH_FIELD` | 2 | object.field [a], [b] keys down: set the item, copying shared levels |
| 113 | `FOR_EACH2` | 3 | next position and item of local [a] into locals [b], [b+1], or jump [c] |
| 114 | `NEW_OBJECT` | 2 | an object of tree constant [a] from its [b] fields on the stack |
| 115 | `GET_FIELD` | 1 | field [a] of the object on top |
| 116 | `SET_FIELD` | 1 | object.field [a] = value (shared: every holder sees it) |
| 117 | `GET_FIELD_L` | 2 | field [b] of the object in local [a] |
| 118 | `SET_FIELD_L` | 2 | pop into field [b] of the object in local [a] |
| 119 | `FIELD_ADD_INT_K` | 3 | int field [b] of the object in local [a] += int constant [c] |
| 120 | `FIELD_SUB_INT_K` | 3 | int field [b] of the object in local [a] -= int constant [c] |
| 121 | `FIELD_ADD_INT_L` | 3 | int field [b] of the object in local [a] += int local [c] |
| 122 | `FIELD_SUB_INT_L` | 3 | int field [b] of the object in local [a] -= int local [c] |
| 123 | `FIELD_ADD_FLOAT_K` | 3 | float field [b] of the object in local [a] += float constant [c] |
| 124 | `FIELD_SUB_FLOAT_K` | 3 | float field [b] of the object in local [a] -= float constant [c] |
| 125 | `FIELD_ADD_FLOAT_L` | 3 | float field [b] of the object in local [a] += float local [c] |
| 126 | `FIELD_SUB_FLOAT_L` | 3 | float field [b] of the object in local [a] -= float local [c] |
| 127 | `IS_TREE` | 1 | is the top value an object of tree constant [a] or one extending it |
| 128 | `WEAK` | 0 | the top value as a weak reference (an object's weak box; none stays none) |
| 129 | `UNWEAK` | 0 | a weak reference's object, or none if it is gone |
| 130 | `IS_TRAIT` | 1 | does the top value's tree have the trait named by constant [a] |
| 131 | `INVOKE` | 2 | call method [a] of the object under [b] arguments, the object as self |
| 132 | `INVOKE_FROM` | 3 | call tree constant [a]'s method [b] (super, init) with [c] arguments |
| 133 | `CALL_METHOD` | 2 | call program method [a], found when compiling, the object under [b] args |
| 134 | `EQ` | 0 | a == b, for any two values |
| 135 | `NE` | 0 | a != b, for any two values |
| 136 | `NOT` | 0 | not a |
| 137 | `IS_NONE` | 0 | a == none |
| 138 | `JUMP` | 1 | jump forward [a] bytes |
| 139 | `JUMP_IF_FALSE` | 1 | pop; jump forward [a] bytes if bad |
| 140 | `JUMP_IF_FALSE_KEEP` | 1 | jump forward [a] bytes if bad, keeping the value |
| 141 | `JUMP_IF_TRUE_KEEP` | 1 | jump forward [a] bytes if good, keeping the value |
| 142 | `JUMP_IF_NOT_NONE_KEEP` | 1 | jump forward [a] bytes unless none, keeping the value |
| 143 | `JUMP_IF_NONE_KEEP` | 1 | jump forward [a] bytes if none, keeping the value |
| 144 | `JUMP_IF_NOT_LT_INT` | 1 | pop two ints; jump forward [a] bytes unless a < b |
| 145 | `JUMP_IF_NOT_LE_INT` | 1 | pop two ints; jump forward [a] bytes unless a <= b |
| 146 | `JUMP_IF_NOT_GT_INT` | 1 | pop two ints; jump forward [a] bytes unless a > b |
| 147 | `JUMP_IF_NOT_GE_INT` | 1 | pop two ints; jump forward [a] bytes unless a >= b |
| 148 | `JUMP_IF_NOT_EQ_INT` | 1 | pop two ints; jump forward [a] bytes unless a == b |
| 149 | `JUMP_IF_NOT_NE_INT` | 1 | pop two ints; jump forward [a] bytes unless a != b |
| 150 | `JUMP_IF_NOT_LT_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a < b |
| 151 | `JUMP_IF_NOT_LE_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a <= b |
| 152 | `JUMP_IF_NOT_GT_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a > b |
| 153 | `JUMP_IF_NOT_GE_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a >= b |
| 154 | `JUMP_IF_NOT_EQ_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a == b |
| 155 | `JUMP_IF_NOT_NE_FLOAT` | 1 | pop two floats; jump forward [a] bytes unless a != b |
| 156 | `JUMP_IF_NOT_LT_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a < constant [a] |
| 157 | `JUMP_IF_NOT_LE_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a <= constant [a] |
| 158 | `JUMP_IF_NOT_GT_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a > constant [a] |
| 159 | `JUMP_IF_NOT_GE_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a >= constant [a] |
| 160 | `JUMP_IF_NOT_EQ_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a == constant [a] |
| 161 | `JUMP_IF_NOT_NE_INT_K` | 2 | pop a (int); jump forward [b] bytes unless a != constant [a] |
| 162 | `JUMP_IF_NOT_LT_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a < constant [a] |
| 163 | `JUMP_IF_NOT_LE_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a <= constant [a] |
| 164 | `JUMP_IF_NOT_GT_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a > constant [a] |
| 165 | `JUMP_IF_NOT_GE_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a >= constant [a] |
| 166 | `JUMP_IF_NOT_EQ_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a == constant [a] |
| 167 | `JUMP_IF_NOT_NE_FLOAT_K` | 2 | pop a (float); jump forward [b] bytes unless a != constant [a] |
| 168 | `JUMP_IF_NOT_LT_INT_LK` | 3 | jump [c] unless int local [a] < [b] |
| 169 | `JUMP_IF_NOT_LE_INT_LK` | 3 | jump [c] unless int local [a] <= [b] |
| 170 | `JUMP_IF_NOT_GT_INT_LK` | 3 | jump [c] unless int local [a] > [b] |
| 171 | `JUMP_IF_NOT_GE_INT_LK` | 3 | jump [c] unless int local [a] >= [b] |
| 172 | `JUMP_IF_NOT_EQ_INT_LK` | 3 | jump [c] unless int local [a] == [b] |
| 173 | `JUMP_IF_NOT_NE_INT_LK` | 3 | jump [c] unless int local [a] != [b] |
| 174 | `LOOP` | 1 | jump back [a] bytes |
| 175 | `FOR_RANGE` | 3 | jump forward [c] if local [a] is past local [a+1] (<, or <= if [b]) |
| 176 | `TRY` | 1 | until END_TRY: on an error, unwind and jump forward [a] with the error pushed |
| 177 | `END_TRY` | 1 | the last [a] tries are over |
| 178 | `THROW` | 0 | throw the error on top |
| 179 | `CALL` | 1 | call the function under [a] arguments |
| 180 | `CALL_GLOBAL` | 2 | call the function in global [a] with the [b] arguments on top |
| 181 | `CALL_SELF` | 1 | call the running function with the [a] arguments on top |
| 182 | `TAIL_SELF` | 1 | return what the running function returns for the [a] arguments on top |
| 183 | `CALL_BUILTIN` | 2 | call built-in [a] with [b] arguments |
| 184 | `SPAWN` | 1 | a task running the function under [a] arguments; push the task |
| 185 | `CALL_NATIVE` | 2 | call standard-library function [a] with [b] arguments |
| 186 | `RETURN` | 0 | return the top value |
| 187 | `RETURN_LOCAL` | 1 | return local slot [a] |
| 188 | `RETURN_ADD_INT` | 0 | return a + b |
| 189 | `RETURN_NONE` | 0 | return none |
<!-- opcodes:end -->
