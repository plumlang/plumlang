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
