# The Plumlang Language Specification

Version 0.1 (draft). This document says what Plumlang programs mean. The
implementation follows it; a change to the language changes this document,
the tests and the implementation in the same pull request (PREAMBLE 2.5).

Sections marked **(Stage N)** describe behaviour that lands with that stage
of the [roadmap](https://github.com/plumlang/plumlang/issues/36); until then
the compiler reports them as not yet supported.

## 1. Design principles

1. **Easy to write comes first.** One clear way to do each thing. When two
   designs are equally powerful, the one a beginner reads correctly wins.
2. **Safe by default.** No null crashes, no out-of-bounds reads, no integer
   wrap-around, no data races, no use-after-free. Mistakes are errors with
   a location and a hint, preferably before the program runs.
3. **Predictable memory.** Values are freed the moment they are no longer
   used. There is no garbage-collector pause and no manual freeing.
4. **Small.** Every feature earns its place.

A first program:

```plum
drop("Hello, world!")
```

A taste of most of the language:

```plum
seed math

root GREETING = "Hello"

tree Planet {
    name: str
    radius_km: float

    branch area() -> float {
        return 4.0 * math.PI * self.radius_km ** 2
    }
}

plant planets = [Planet("Mercury", 2439.7), Planet("Earth", 6371.0)]

for p in planets {
    drop("{GREETING}, {p.name}! Surface: {p.area()} km²")
}
```

## 2. Source text

### 2.1 Files

A Plumlang source file is UTF-8 text with the extension `.plum`. A byte
order mark at the start is ignored. Line endings may be `\n` or `\r\n`.

### 2.2 Comments

A comment starts with `#` and runs to the end of the line.

```plum
# A whole-line comment
plant total = 0   # a comment after code
```

There are no block comments.

### 2.3 Statements and lines

A statement ends at the end of its line. There are no semicolons.

A statement continues onto the next line when the line ends inside an open
`(`, `[` or `{`, or ends with a binary operator, `,`, `.` or `->`:

```plum
plant total = price
    + tax
    + shipping

plant names = [
    "Ada",
    "Grace",
]
```

A trailing comma is allowed in every list of items.

### 2.4 Identifiers

An identifier starts with a letter or `_` and continues with letters,
digits and `_`. Letters are ASCII `a`–`z` and `A`–`Z`. Identifiers are
case-sensitive.

By convention, which the formatter and the linter follow:

| Kind | Style | Example |
|---|---|---|
| Variables, functions, fields, modules | `snake_case` | `total_price`, `read_file` |
| Constants | `UPPER_SNAKE_CASE` | `MAX_USERS` |
| Types (`tree`, `trait`, `kind`) | `PascalCase` | `HttpRequest` |

A name starting with `_` is private to its module or tree (section 9.4).

### 2.5 Keywords

These words are reserved and cannot be used as identifiers:

| Keyword | Meaning |
|---|---|
| `plant` | declare a variable |
| `root` | declare a constant |
| `branch` | declare a function or method |
| `tree` | declare a class |
| `trait` | declare an interface |
| `kind` | declare an enumeration |
| `seed` | import a module |
| `good`, `bad` | the boolean values true and false |
| `none` | the absent value |
| `if`, `else`, `match` | choose |
| `for`, `in`, `while`, `break`, `continue` | repeat |
| `return` | leave a function |
| `and`, `or`, `not` | logic |
| `is` | type test |
| `as` | conversion, import alias |
| `self`, `super` | the current object, its parent tree |
| `throw`, `try`, `catch` | errors |
| `spawn` | start a task |
| `weak` | a non-owning reference |

`true` and `false` are also accepted as spellings of `good` and `bad`, so
programs from other languages read naturally. The formatter keeps whichever
spelling a file uses.

### 2.6 Literals

**Integers** are written in decimal, hexadecimal (`0x`), octal (`0o`) or
binary (`0b`). `_` may separate digits.

```plum
42    1_000_000    0xFF    0o755    0b1010_0101
```

An integer literal that does not fit in 64 bits is a compile error.

**Floats** have a `.` with digits on both sides, an exponent, or both.

```plum
3.14    0.5    1e9    6.022e23    2.5e-3
```

`.5` and `5.` are not floats; write `0.5` and `5.0`.

**Booleans** are `good` and `bad` (or `true` and `false`).

**Strings** are written in double quotes. There is one string syntax.

```plum
"Hello"
"Tab:\t, newline:\n, quote: \", backslash: \\"
"Unicode: \u{1F351}"
```

| Escape | Meaning |
|---|---|
| `\n` `\t` `\r` `\0` | newline, tab, carriage return, NUL |
| `\\` `\"` | backslash, double quote |
| `\{` `\}` | literal braces |
| `\u{XXXX}` | a Unicode code point, 1 to 6 hex digits |

Any other escape is a compile error.

**Interpolation**: `{expression}` inside a string is replaced by the
expression's text (section 4.10).

```plum
plant name = "Ada"
drop("Hello, {name}! 2 + 2 = {2 + 2}")
```

**Multi-line strings** use triple quotes. The first newline after the
opening `"""` is dropped, and the indentation of the closing `"""` is
removed from every line:

```plum
plant poem = """
    Roses are red,
    Plums are purple.
    """
# poem == "Roses are red,\nPlums are purple.\n"
```

**Lists** `[1, 2, 3]`, **maps** `{"a": 1, "b": 2}`, and the empty map
`{:}` are described in section 3.4.

## 3. Types

Plumlang is statically typed: every expression has a type known before the
program runs, and type mistakes are compile errors. Most types are inferred,
so they rarely need writing.

### 3.1 Basic types

| Type | Values | Notes |
|---|---|---|
| `int` | 64-bit signed integers | Overflow is a runtime error, never wrap-around |
| `float` | 64-bit IEEE 754 numbers | |
| `bool` | `good`, `bad` | |
| `str` | immutable UTF-8 text | |
| `byte` | integers 0 to 255 | For binary data, in `list[byte]` |
| `none` | the single value `none` | The type of functions that return nothing |

### 3.2 Conversions

There is exactly one implicit conversion: **an `int` becomes a `float`**
where a `float` is expected, including in arithmetic that mixes them.

```plum
plant x: float = 5      # 5.0
plant y = 2 * 1.5       # 3.0, a float
```

Every other conversion is written:

| Conversion | Result |
|---|---|
| `int(3.9)` | `3` (truncates toward zero; error if out of range or NaN) |
| `float(3)` | `3.0` |
| `str(42)`, `str(1.5)`, `str(good)` | `"42"`, `"1.5"`, `"good"` |
| `int("42")`, `float("1.5")` | parse, or throw a `ParseError` |
| `bool(x)` | not allowed: write the comparison you mean |

There is no truthiness: `if count {` is a type error; write `if count > 0 {`.

### 3.3 Optional types

`T?` is either a `T` or `none`. Only optional types can hold `none`, so a
plain `str` is never absent, and nothing crashes on a missing value.

```plum
plant nickname: str? = none

drop(nickname ?? "no nickname")           # default when none

if plant n = nickname {                   # unwrap
    drop("Called {n}")                    # n is a str here
}

drop(nickname?.upper())                   # none-safe call: a str?
```

### 3.4 Collections

| Type | Literal | Notes |
|---|---|---|
| `list[T]` | `[1, 2, 3]`, `[]` | Ordered, growable. Index from 0 |
| `map[K, V]` | `{"a": 1}`, `{:}` | Keys are `int`, `str`, `bool`, `byte` or a `kind`; insertion order is kept |
| `set[T]` | `set([1, 2])` | Unique values, insertion order kept |

The empty literals `[]` and `{:}` need a type from context:

```plum
plant names: list[str] = []
plant ages: map[str, int] = {:}
```

Reading a list out of bounds, or a missing map key with `m[k]`, is a
runtime error; `m.get(k)` returns `V?` instead.

### 3.5 Function types

`(int, int) -> int` is the type of a function taking two `int`s and
returning an `int`. `() -> none` takes nothing and returns nothing.

### 3.6 User-defined types

`tree` declares a class (section 8), `trait` an interface (section 8.5),
and `kind` an enumeration (section 8.6).

### 3.7 Type annotations

Annotations follow a name after `:`. They are needed on function
parameters, function results other than `none`, tree fields, and where
inference has nothing to go on (empty collections, `none`).

```plum
plant count = 0                  # int, inferred
plant ratio: float = 1           # float
plant scores: list[int] = []
```

## 4. Variables and expressions

### 4.1 Declarations

`plant` declares a variable, `root` a constant. A declaration always has a
value.

```plum
plant score = 0
score = score + 10        # fine: plant can change

root MAX_SCORE = 100
MAX_SCORE = 200           # compile error: MAX_SCORE is a root
```

Using a variable before it is declared is a compile error. Declaring the
same name twice in one block is a compile error; an inner block may shadow
an outer name.

### 4.2 Scope

A name is visible from its declaration to the end of the block `{ }` that
contains it. Names declared at the top level of a file belong to the module
(section 9).

### 4.3 Assignment

`=` assigns to a `plant` variable, a list element, a map entry or a field.
Assignment is a statement, not an expression: `if x = 5 {` is a syntax
error.

Compound assignment applies an operator and assigns:
`+=` `-=` `*=` `/=` `//=` `%=` `**=` `&=` `|=` `^=` `<<=` `>>=`.
`x += 1` means `x = x + 1`, with the same type rules. There is no `++` or
`--`.

### 4.4 Arithmetic

| Operator | Meaning | Types |
|---|---|---|
| `a + b` | add | numbers; `str + str` and `list + list` concatenate |
| `a - b` | subtract | numbers |
| `a * b` | multiply | numbers |
| `a / b` | divide | numbers; **always a `float`**: `7 / 2 == 3.5` |
| `a // b` | floor division | `int // int` is an `int`: `7 // 2 == 3`, `-7 // 2 == -4` |
| `a % b` | remainder, same sign as `b` | numbers: `-7 % 2 == 1` |
| `a ** b` | power | `int ** int` is an `int` (exponent must be ≥ 0); otherwise `float` |
| `-a` | negate | numbers |

Integer overflow, `//` or `%` by zero, and `int ** negative` are runtime
errors. Float division by zero follows IEEE 754 (`inf`, `nan`).

### 4.5 Comparison

`==` `!=` `<` `<=` `>` `>=`. Both sides must have the same type, except that
`int` and `float` compare by value. Strings compare by Unicode code point;
lists and maps compare element by element with `==`. Trees compare by
identity unless they implement `Equal` (section 8.5).

Comparisons do not chain: `a < b < c` is a syntax error; write
`a < b and b < c`.

### 4.6 Logic

`and`, `or` and `not` take and give `bool`. `and` and `or` evaluate the
right side only when needed.

```plum
if age >= 18 and not banned {
    drop("Welcome")
}
```

### 4.7 Bitwise

On `int` (and `byte`) only: `&` and, `|` or, `^` xor, `~` not, `<<` and
`>>` shifts. A shift count must be between 0 and 63; `>>` is arithmetic.

### 4.8 Precedence

From lowest to highest; operators on one row group left to right, except
`**`, which groups right to left.

| Level | Operators |
|---|---|
| 1 | `??` |
| 2 | `or` |
| 3 | `and` |
| 4 | `not` |
| 5 | `==` `!=` `<` `<=` `>` `>=` `is` `in` |
| 6 | `..` `..=` |
| 7 | `\|` |
| 8 | `^` |
| 9 | `&` |
| 10 | `<<` `>>` |
| 11 | `+` `-` |
| 12 | `*` `/` `//` `%` |
| 13 | unary `-` `~` |
| 14 | `**` |
| 15 | `as` |
| 16 | call `f()`, index `a[i]`, field `a.b`, `a?.b` |

So `-2 ** 2 == -4`, `1 + 2 * 3 == 7`, and `0..n + 1` is `0..(n + 1)`.
Bitwise operators bind tighter than comparisons, as in Rust, Go and Swift:
`flags & MASK == 0` means `(flags & MASK) == 0`, avoiding the C trap.

### 4.9 Membership and type tests

`x in collection` tests membership in a list, set, map (keys) or string
(substring). `value is Type` tests the runtime type of a trait or optional
value.

### 4.10 Interpolation

`"{expr}"` converts the value with `str()`. A tree converts with its
`to_str()` method if it has one (trait `Show`), else as `<TreeName>`.
Formatting options follow a `:`:

```plum
drop("{price:.2}")        # 2 digits after the point
drop("{count:5}")         # right-aligned in 5 columns
drop("{name:<10}|")       # left-aligned
drop("{n:x}  {n:b}")      # hex, binary
```

### 4.11 Ranges

`a..b` is the integers from `a` up to but not including `b`; `a..=b`
includes `b`. Ranges are used in `for` loops and slices:

```plum
for i in 0..5 { }          # 0 1 2 3 4
plant middle = items[1..3] # a new list of items 1 and 2
plant word = text[0..5]    # characters 0 to 4
```

## 5. Statements

### 5.1 Blocks

A block is a sequence of statements in `{ }`. The opening brace goes on the
same line as the statement that owns it.

### 5.2 if

```plum
if score >= 90 {
    drop("A")
} else if score >= 80 {
    drop("B")
} else {
    drop("C")
}
```

The condition must be a `bool`. No parentheses are needed around it.

`if` is also an expression when both branches are single expressions:

```plum
plant label = if count == 1 { "item" } else { "items" }
```

### 5.3 match

`match` picks the first arm whose pattern fits. It must cover every case;
`else` covers the rest.

```plum
match command {
    "start" => start()
    "stop", "halt" => stop()
    else => drop("Unknown command: {command}")
}

match shape {
    Circle(r) => drop("circle of radius {r}")
    Square(side) => drop("square of side {side}")
}
```

Patterns are literals, `kind` cases with their values (section 8.6), ranges
(`0..10`), and `_`, which matches anything. `match` is also an expression
when every arm is a single expression.

### 5.4 while

```plum
plant n = 10
while n > 0 {
    n -= 3
}
```

### 5.5 for

`for` walks over a range, a list, a set, a map, a string (its characters)
or anything with an `iter()` method (trait `Iterable`).

```plum
for i in 0..10 { drop(i) }

for name in names { drop(name) }

for i, name in names.enumerate() { drop("{i}: {name}") }

for key, value in ages { drop("{key} is {value}") }
```

The loop variable is a new constant on each iteration. Changing a
collection while looping over it is a runtime error.

There is no C-style `for (init; condition; step)`; use a range or `while`.

### 5.6 break and continue

`break` leaves the innermost loop; `continue` starts its next iteration.
Outside a loop they are compile errors.

### 5.7 Expression statements

A call can stand on its own as a statement. Any other expression standing
alone (`x + 1`) is a compile error, since its value would be lost.
