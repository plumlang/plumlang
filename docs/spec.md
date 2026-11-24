# The Plumlang Language Specification

Version 0.1 (draft). This document says what Plumlang programs mean. The
implementation follows it; a change to the language changes this document,
the tests and the implementation in the same pull request (PREAMBLE 2.5).

Sections marked **(Stage N)** describe behaviour that lands with that stage
of the [roadmap](https://github.com/plumlang/plumlang/issues/36); until then
the compiler reports them as not yet supported. Sections marked **(planned
for 3.0)** are the agreed design for a later release: nothing in them is
implemented yet, and their examples don't run with today's `plum`.

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

Code may nest up to 1000 deep: brackets, blocks, types and lambdas inside
each other, with each operator of a chain like `a + b + c` counting one
level. Deeper code is a compile error (E0218), never a crash. Other limits,
each a compile error: a function, lambda or kind case has at most 255
parameters (E0219); a tree has at most 255 fields with its parents'
(E0429); a tree or trait extends trees and traits at most 32 deep (E0428),
and never itself (E0427).

### 2.2 Comments

A comment starts with `#` and runs to the end of the line.

```plum
# A whole-line comment
plant total = 0   # a comment after code
```

A **block comment** starts with `#[` and ends with `]#`. It may span lines
and may sit anywhere a space may. Block comments nest: each `#[` inside
one needs its own `]#`, so code that already holds a block comment can be
commented out with another. Nothing else inside a block comment means
anything, strings and `#` included: the first `]#` that matches closes
it. A block comment counts as a space, so a line break inside one doesn't
end a statement. One that is never closed is a compile error at the `#[`
that opened it (E0140).

```plum
#[
plant old = compute()
#[ an earlier note ]#
drop(old)
]#
plant area = width * #[ not height: ]# depth
```

`#[` and `]#` inside a string are text, and outside a block comment `]#`
is an ordinary `]` and the start of a comment.

A **doc comment** is a line comment that starts with exactly `##` (`###`
is an ordinary comment). The `##` lines directly before a declaration
(`branch`, `tree`, `trait`, `kind`, `root`, a field, a method or a kind
case), with no blank line or other comment between, document it. A
file's first `##` lines, with only blank lines and other comments before
them, document the module, unless they are directly before a
declaration. Each line's text, without the `##` and one space after it,
is Markdown. The language server shows it on hover, and documentation
tools read it. A `##` line anywhere else is an ordinary comment.

```plum
## Shapes and their areas.

seed math

## The area of a circle of radius `r`.
##
## Negative radii are treated as positive.
branch circle_area(r: float) -> float {
    return math.PI * r ** 2
}

tree Pot {
    ## Litres of soil.
    size: float
}
```

### 2.3 Statements and lines

A statement ends at the end of its line. There are no semicolons.

A statement continues onto the next line when the line ends inside an open
`(`, `[` or `{`, or ends with a binary operator, `,`, `.` or `->`:

```plum
plant total = price +
    tax +
    shipping

plant names = [
    "Ada",
    "Grace",
]
```

The operator goes at the end of the line, not the start of the next one: a
line that starts with `+` is a new statement, and `+ tax` on its own is an
error (E0201).

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
| `throw`, `try`, `catch`, `finally` | errors |
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

### 3.2.1 How values print

`str()`, `drop` and interpolation turn values into text the same way:

| Value | Text |
|---|---|
| `int` | decimal digits: `42`, `-7` |
| `float` | the shortest text that reads back as the same number, always with a `.` or an exponent. Exponents from −4 to 15 are written out (`1.0`, `0.0025`, `1000000000.0`), others in scientific notation (`1e+20`, `1e-05`); also `inf`, `-inf`, `nan` |
| `bool` | `good` or `bad` |
| `str` | the text itself, without quotes |
| `none` | `none` |
| list, map, set | `[1, 2]`, `{"a": 1}`, `set([1, 2])`, with strings inside quoted |
| tree | its `to_str()` (trait `Show`), else `<TreeName>` |

Values nested more than 256 deep (a long chain of kind values, say) print
their outer 256 levels and `...` for what is inside, so printing never
runs out of stack. The depth counts through `to_str()` methods that print
values of their own.

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

`+ - * /` and `-a` also work on objects whose tree has the matching
method (`plus`, `minus`, `times`, `divided_by`, `negate`; section 8.5).

### 4.5 Comparison

`==` `!=` `<` `<=` `>` `>=`. Both sides must have the same type, except that
`int` and `float` compare by value. Strings compare by Unicode code point;
lists and maps compare element by element with `==`. Trees compare by
identity unless they implement `Equal` (section 8.5). `<` `<=` `>` `>=`
work on numbers, on strings, and on objects whose tree implements
`Order` (a `compare` method, section 8.5).

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
includes `b`. A range is written where it is used, in a `for` loop, a
slice, after `in`, or as a `match` pattern; it is not a value that can be
stored or passed:

```plum
for i in 0..5 { }          # 0 1 2 3 4
plant middle = items[1..3] # a new list of items 1 and 2
plant word = text[0..5]    # characters 0 to 4
if age in 13..=19 { }      # 13 <= age and age <= 19
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

The loop variable is a new constant on each iteration. A loop goes over
the collection as it was when the loop started: since collections are
values (section 11.1), changing the variable inside the loop changes the
variable, not the loop.

```plum
plant xs = [1, 2]
for x in xs { xs.push(x) }   # runs twice; xs is then [1, 2, 1, 2]
```

There is no C-style `for (init; condition; step)`; use a range or `while`.

### 5.6 break and continue

`break` leaves the innermost loop; `continue` starts its next iteration.
Outside a loop they are compile errors.

### 5.7 Expression statements

A call can stand on its own as a statement. Any other expression standing
alone (`x + 1`) is a compile error, since its value would be lost.

## 6. Functions

### 6.1 Declaring and calling

```plum
branch add(a: int, b: int) -> int {
    return a + b
}

branch greet(name: str) {           # returns none
    drop("Hello, {name}")
}

drop(add(2, 3))
greet("Ada")
```

Parameter types are always written. The result type is written after `->`
and left out for functions that return nothing. A function whose result is
not `none` must `return` a value on every path; the compiler checks.

A function body that is a single expression can use `=>`:

```plum
branch square(x: int) -> int => x * x
```

Functions can be called before their declaration in the same module. A
function may use module variables declared anywhere in the module, but
using one before its `plant` or `root` line has run (a call made above
the declaration) is a runtime `Error`, not a value of the wrong type:

```plum
branch greet() -> str => "Hello, {name}"
drop(greet())        # Error: a module variable was used before its `plant` line ran
plant name = "Ada"
drop(greet())        # Hello, Ada
```

Using a module variable above its declaration outside a function is a
compile error (E0303).

### 6.2 Default and named arguments

```plum
branch connect(host: str, port: int = 80, secure: bool = bad) { }

connect("example.com")
connect("example.com", 443, secure: good)
connect("example.com", secure: good)
```

Defaults must be constants or literals. Named arguments come after
positional ones.

### 6.3 Parameters are read-only

Parameters are constants inside the function. Because strings, lists and
maps are values (section 11), a function cannot change the caller's list;
it returns a new one, or takes a tree, whose objects are shared.

### 6.4 Function values and closures

Functions are values. A lambda is written `(params) => expression` or
`(params) => { statements }`:

```plum
plant double = (x: int) => x * 2
plant evens = numbers.filter((n) => n % 2 == 0)
plant sorted = people.sorted_by((p) => p.age)
```

Lambda parameter types can be left out when the context gives them, as in
`filter` above. A lambda captures the variables it uses; it sees their
current values (a captured `plant` can be changed by the lambda only if it
is declared in an enclosing function, not across tasks).

### 6.5 Recursion

Functions may call themselves. Deep recursion past the stack limit (by
default 10,000 frames) is a runtime error, not a crash. Calls made from
inside the runtime (a `to_str` running while something prints, the
function given to `map` or `filter`) may nest 100 deep; past that is also
a runtime error.

A declared function that returns a call of itself by name,
`return f(...)` (or `=> f(...)`), outside any `try`, is a *tail call*:
the new call takes the place of the running one instead of going on top
of it. Such a function can recurse any number of times without reaching
the stack limit, and a stack trace shows the call once, with how many
tail calls replaced it:

```text
count.plum:3: ZeroDivisionError: division by zero: 1 // 0
  in count, line 3 (after 50 tail calls)
  in <main>, line 7
```

A call that isn't the whole value of the `return` (`return 1 + f(n - 1)`),
a call through a variable or a method, and any call inside a `try` (whose
`catch` must still be there when it fails) are ordinary calls.

A call of a small declared function whose body is one expression that
can't fail (comparisons, `and`, `or`, `not`, float arithmetic, `&`, `|`,
`^` and `~` on its `int`, `float` and `bool` parameters) may be compiled
in place of the call. Such a call never appears in a stack trace, since
nothing in it can throw, and it counts neither toward the stack limit nor
toward a limit on the steps a run may take.

## 7. Built-in functions

These are always available without `seed`:

| Function | Meaning |
|---|---|
| `drop(values...)` | print the values separated by spaces, then a newline |
| `ask(prompt: str = "") -> str?` | print the prompt, read a line from standard input; `none` at end of input |
| `len(x) -> int` | length of a string (in characters), list, map or set |
| `int`, `float`, `str`, `byte` | conversions (section 3.2) |
| `type_of(x) -> str` | the type's name, for messages and debugging |
| `assert(cond: bool, message: str = "")` | throw an `AssertError` if `cond` is `bad` |
| `exit(code: int = 0)` | end the program |
| `freeze(x)` | a frozen copy of `x`, which every worker can read at once (planned for 3.0, section 12.7) |

`drop` is an ordinary function, not a statement: it is always called with
parentheses, `drop("hi")`.

## 8. Trees: classes and objects (Stage 5)

### 8.1 Declaring a tree

A `tree` groups fields and the methods that work on them:

```plum
tree Account {
    owner: str
    balance: float = 0.0

    branch deposit(amount: float) {
        if amount <= 0 {
            throw ValueError("deposit must be positive")
        }
        self.balance += amount
    }

    branch to_str() -> str => "{self.owner}: {self.balance:.2}"
}

plant acct = Account("Ada")          # balance takes its default
acct.deposit(50)
drop(acct)                           # Ada: 50.00
```

Inside a method, `self` is the object; fields and methods are always
reached through it (`self.balance`), so a field is never confused with a
local variable.

### 8.2 Construction

Calling the tree's name creates an object. By default the arguments fill
the fields in declaration order; fields with defaults may be left out or
named: `Account("Ada", balance: 10.0)`.

A tree may define `init` to construct objects differently. When it does,
calls to the tree's name call `init`. Fields with defaults are set first;
`init` must then set every field without a default, in its opening
statements (`self.field = value`), before anything else uses `self`. A
tree that inherits `init` and adds a field without a default needs its
own `init`.

```plum
tree Temperature {
    celsius: float

    branch init(fahrenheit: float) {
        self.celsius = (fahrenheit - 32.0) * 5.0 / 9.0
    }
}
```

### 8.3 Objects are shared

Tree objects are reference values: assigning or passing one shares the same
object, and changes through one name are seen through every name. Every
other value in Plumlang is copied (section 11). For a tree without
`equals`, `a == b` asks whether two names refer to the same object
(section 4.5).

### 8.4 Inheritance

A tree can extend one other tree. It inherits the fields and methods, adds
its own, and can override methods. `super.method()` calls the parent's
version.

```plum
tree Animal {
    name: str
    branch speak() -> str => "..."
}

tree Dog : Animal {
    branch speak() -> str => "{self.name} says woof"
}

plant pets: list[Animal] = [Dog("Rex"), Animal("Generic")]
for pet in pets { drop(pet.speak()) }
```

A `Dog` can be used wherever an `Animal` is expected. There is no multiple
inheritance; use traits.

### 8.5 Traits

A `trait` lists methods a tree promises to have. A tree implements traits
by naming them after `:`, after its parent tree if it has one:

```plum
trait Shape {
    branch area() -> float
    branch name() -> str => "shape"     # a default method
}

tree Circle : Shape {
    radius: float
    branch area() -> float => 3.14159 * self.radius ** 2
}

branch total_area(shapes: list[Shape]) -> float {
    plant sum = 0.0
    for s in shapes { sum += s.area() }
    return sum
}
```

Missing methods are compile errors, and a tree's version of a trait's
method (or of a parent's method it overrides) must have exactly the same
parameters and result. `x is Shape` asks whether an object's tree has the
trait.

Some methods the language calls itself:

- `to_str() -> str` is how an object prints: in `drop`, `str()` and
  interpolation, and inside a printed list or map. Without it, an object
  prints as `<TreeName>`.
- `equals(other: T) -> bool`, where `T` is the tree or one it extends, is
  what `a == b` and `a != b` mean when `a`'s type is a tree with
  `equals` (trait `Equal`). Without it, `==` asks whether both are the
  same object. In a list whose item type has `equals` (also lists of
  such lists), `contains`, `index_of`, `x in list` and `==` between
  lists use it too, calling each item's `equals`. Elsewhere in
  collections (map keys and values, sets, a list of a parent tree whose
  subclass alone has `equals`) objects compare by identity.
- `compare(other: T) -> int` (trait `Order`) is what `a < b`, `a <= b`,
  `a > b` and `a >= b` mean: it returns a negative number if `self`
  comes before `other`, zero if neither does, and a positive number if
  `self` comes after. A list whose item type has `compare` can `sort()`
  and `sorted()`; the sort is stable. A `compare` that throws stops the
  sort with its error.
- The arithmetic operators call these methods (on the left object):

  | Operator | Method |
  |---|---|
  | `a + b` | `plus(other: T) -> T` |
  | `a - b` | `minus(other: T) -> T` |
  | `a * b` | `times(other: T) -> T`, or `times(k: int)` / `times(k: float)` |
  | `a / b` | `divided_by(other: T) -> T`, or `divided_by(k: int)` / `divided_by(k: float)` |
  | `-a` | `negate() -> T` |

  `T` is the tree or one it extends (or a trait it has), and the result
  is too; `times` and `divided_by` may take a number instead, to scale.
  Compound assignment uses them as well: `v += w` is `v = v + w`. The
  object must be on the left: `v * 2.0` works, `2.0 * v` doesn't.

```plum
tree Vector {
    x: float
    y: float
    branch plus(other: Vector) -> Vector => Vector(self.x + other.x, self.y + other.y)
    branch times(k: float) -> Vector => Vector(self.x * k, self.y * k)
    branch negate() -> Vector => Vector(-self.x, -self.y)
}

plant v = Vector(1.0, 2.0) + Vector(3.0, 4.0) * 2.0    # (7.0, 10.0)
v += -v                                                # (0.0, 0.0)
```

These methods are found in the object's own tree, so a subclass that
overrides one changes what the operator does, even where the object's
type is the parent. Their shapes are fixed (E0424), and an override has
the same parameters and result as the method it overrides (E0421).

`Hash` (`hash`, for map keys) and `Iterable` (`iter`, for `for`) come
with the standard library.

Fields and methods whose names start with `_` are private to the tree and
the trees that extend it (section 9.4).

### 8.6 Kinds: enumerations

A `kind` is a type with a fixed set of cases. Cases may carry values.

```plum
kind Direction { North, East, South, West }

kind Shape {
    Circle(radius: float)
    Rect(width: float, height: float)
}

branch area(s: Shape) -> float => match s {
    Circle(r) => 3.14159 * r ** 2
    Rect(w, h) => w * h
}

plant heading = Direction.North
```

`match` on a kind must cover every case.

A kind's values are values like numbers and strings: two are equal when
they are the same case with equal contents (`Shape.Rect(2.0, 3.0) ==
Shape.Rect(2.0, 3.0)`), and they print as written: `Direction.North`,
`Shape.Circle(1.0)`. `type_of` gives the kind's name.

## 9. Modules (Stage 5)

### 9.1 Files are modules

Every `.plum` file is a module. Its top-level statements run once, in order,
the first time it is imported, or when it is the program being run. A
program needs no `main` function. A module's functions exist before its
first statement runs; its variables exist once their declaration has run
(section 6.1).

### 9.2 seed

`seed` imports a module, by standard-library name or by path relative to the
importing file:

```plum
seed math                       # standard library: math.sqrt(2.0)
seed net.http                   # use as http.server()
seed net.http as web            # use as web.server()
seed ./shapes                   # the file shapes.plum next to this one
seed ./models/user as user      # models/user.plum
seed math { sqrt, PI }          # bring names in directly: sqrt(PI)
```

A module's name is the last part of its path. Imports must come before other
statements. Circular imports are a compile error that shows the cycle (E0311).

With `seed ./shapes`, the module's names are reached through it:
`shapes.area(c)`, and in types `shapes.Circle`. With `seed ./shapes {
Circle, area }`, those names are used directly. Either way the names are the
module's own: a variable it changes is changed for everyone who imported it.
Its kinds' cases can be used bare in `match` patterns (`Round =>`).

Each file is loaded once, however many modules import it, and its top-level
statements run once, before the statements of the first module that imports
it. A file may be named like a keyword (`seed ./models/plant`).

### 9.3 Packages

A directory with a `plum.toml` is a package; its `entry` file is the
program, so `plum run my-project` runs it.

```toml
[package]
name = "weather-api"
version = "0.1.0"
entry = "main.plum"
lib = "lib.plum"          # what `seed weather_api` imports (default: entry)

[dependencies]
colors = "^1.0"
```

Packages published on Orchard, Plumlang's registry, are added with
`plum add NAME` and seeded by name. A name that isn't a standard module
(nor under one, like `net.x`) is a package: `seed colors` imports the
package's `lib` file, and `seed colors.palette.dark` its file
`palette/dark.plum`. A `-` in a package's name is written `_` in `seed`
(`seed weather_api` for `weather-api`). The module's name is the last part,
as for other modules.

A module may seed only the packages its own `plum.toml` lists under
`[dependencies]` (E0908); which version is used comes from the project's
`plum.lock`, and a declared package that isn't installed is E0909. The
project is the nearest directory with a `plum.toml` above the file being
run. Version ranges, the lock file and the commands are described in
[orchard.md](orchard.md).

### 9.4 Visibility

Every top-level name is visible to modules that import it, except names
starting with `_`, which are private to their module. Fields and methods
starting with `_` are private to their tree (and the trees that extend it).

## 10. Errors (Stage 5)

### 10.1 Two kinds of mistakes

**Compile errors** (wrong types, unknown names, syntax) stop the program
before it starts, with the file, line, column, what is wrong and a hint.

**Runtime errors** happen while the program runs: a file that cannot be
opened, a list index out of range, integer overflow. A runtime error that
is not caught ends the program with a message and a stack trace, and exit
code 1. The message names the error's tree, so you know what to catch:

```text
bank.plum:8: Overdrawn: not enough money
  in withdraw, line 8
  in pay, line 13
  in <main>, line 16
```

### 10.2 throw, try, catch

Errors are trees that extend the built-in `Error` tree, which has a
`message: str`.

```plum
tree NotFound : Error {
    path: str
}

branch load(path: str) -> str {
    if not fs.exists(path) {
        throw NotFound("no such file", path)
    }
    return fs.read(path)
}

try {
    plant text = load("config.txt")
    drop(text)
} catch e: NotFound {
    drop("Missing: {e.path}")
} catch e {
    drop("Something else went wrong: {e.message}")
}
```

`catch e: Type` catches that error tree and the trees that extend it; a
plain `catch e` catches every error, and `e` is an `Error`. Catches are
tried in order, and an error none of them takes goes on to the next `try`
out. An error thrown inside a `catch` also goes to the next `try` out.

A `try` may end with `finally { }`, which runs however the `try` ends:
normally, through a catch, with an error no catch took (which then carries
on), or by `return`, `break` or `continue`. An error thrown inside `finally`
replaces the one in flight. `exit()` ends the program at once, without
running `finally` blocks.

Rethrowing a caught error with `throw e` keeps the place it was first
thrown, so the stack trace still points at the real problem.

`try` is also an expression that turns an error into `none`:

```plum
plant port = try int(text) ?? 8080
```

The built-in error trees include `ValueError`, `ParseError`, `IndexError`,
`KeyError`, `OverflowError`, `ZeroDivisionError`, `IOError`, `NetError`
and `AssertError`. The errors the language itself raises are these:

| Error | Raised by |
|---|---|
| `OverflowError` | int arithmetic that doesn't fit in 64 bits; `int(f)` of a float too large |
| `ZeroDivisionError` | `//` or `%` by an int zero |
| `IndexError` | a list or string index or slice out of range; `pop()` on an empty list; `insert`/`remove_at` out of range |
| `KeyError` | `m[k]` for a key not in the map |
| `ParseError` | `int(text)` or `float(text)` of text that isn't a number |
| `ValueError` | `byte(n)` out of 0 to 255; an int power with a negative exponent; a shift count out of 0 to 63; a format that doesn't fit the value; a `str` or list larger than it may be (11.4) |
| `AssertError` | `assert(condition)` when the condition is `bad` |
| `Error` | too much recursion; a module variable used before its declaration ran (6.1) |

## 11. Values and memory

### 11.1 Value types and reference types

Numbers, booleans, `none`, strings, lists, maps, sets and kinds are
**values**: assigning or passing one gives the receiver its own copy, so
changing one never changes the other.

```plum
plant a = [1, 2, 3]
plant b = a
b.push(4)
drop(a)        # [1, 2, 3]
drop(b)        # [1, 2, 3, 4]
```

Copies are cheap: the data is shared until one side changes it, and only
then copied (copy-on-write).

Tree objects are **references**: assigning or passing one shares the
object (section 8.3).

### 11.2 Memory is reclaimed automatically and at once

Every value is freed as soon as nothing refers to it anymore, by reference
counting. There is no garbage collector, so there are no pauses, and
memory use follows the program. Files and sockets close when their last
reference goes away, as well as when `close()` is called.

### 11.3 Cycles and weak references

Two tree objects that refer to each other keep each other alive. Break the
cycle by making the back-reference `weak`; a weak reference does not keep
its object alive and reads as `none` once the object is gone, so its type is
optional:

```plum
tree Node {
    children: list[Node] = []
    parent: weak Node? = none
}
```

`weak` goes on a field whose type is an optional tree or trait (E0425).
Assigning an object to a weak field doesn't keep it alive; reading the
field gives the object while something else holds it, and `none` after.

`plum run --check-leaks program.plum` reports, when the program ends, the
tree objects still alive because they are in a cycle, by tree and the line
that made them, and exits with code 1 if there are any:

```text
leak check: 2 objects are still alive in reference cycles:
  1 Pair, made on line 29
  1 Pair, made on line 30
  hint: make one direction of each cycle weak, like 'parent: weak Node? = none'
```

### 11.4 What the runtime guarantees

No Plumlang program can read or write memory it does not own: every index
is checked, there are no raw pointers, and objects are never used after
they are freed. A `str` holds at most 1 GiB (2^30 bytes) and a list at
most 2^27 (134,217,728) items; making a larger one (with `+`, `repeat`,
`join`, `push`...) throws a `ValueError` before any memory is taken. A
program that runs out of memory otherwise ends with `plum: error: out of
memory` and exit code 1. Values may nest as deep as memory allows: comparing,
copying and freeing them doesn't recurse in the runtime, and printing
stops at a depth (section 3.2.1).

### 11.5 Measuring memory

`plum run --mem-stats program.plum` prints to standard error, when the
program ends, how much memory the run used:

```text
memory used by this run (--mem-stats):
  peak in use          82,456 bytes (80.5 KB)
  allocated in all     167,008 bytes (163.1 KB)
  still in use at exit 0 bytes
  allocations          3,014
  resizes              12
  frees                3,014
values made:
  strings              2,001
  lists                1
  maps                 0
  ...
```

Bytes are the sizes of the blocks the C library hands out, counted from
the start of the run to its end (compiling comes before and isn't
counted). "Still in use at exit" is what the run allocated and didn't
free: more than 0 means objects left alive in reference cycles (find them
with `--check-leaks`). Values made are counted by kind: strings, lists,
maps, sets, tree objects, kind values, closures, captured variables, weak
references, tasks and channels. Without `--mem-stats` nothing is counted
and the run costs the same.

## 12. Tasks and concurrency (Stage 5)

### 12.1 spawn

`spawn` runs a function call as a **task**, a lightweight thread of work.
The program keeps going without waiting for it.

```plum
plant job = spawn download("https://example.com/a.txt")
drop("downloading...")
plant text = job.wait()          # wait for the result (or its error)
```

`spawn` returns a `Task[T]`, where `T` is the call's result type.
`task.wait()` returns the result, or throws the task's error.

### 12.2 No async/await

Calls that wait for the outside world (network, files, timers, `ask`)
pause only the task that made them; other tasks keep running. Code is
written the same way whether or not it runs in a task: there is no
`async`, no `await`, and no separate "async" version of any library.

### 12.3 Sharing between tasks

Tasks do not share variables. A task gets its own copy of the values it is
given, and tree objects cannot be passed to a task (a compile error,
E0426). Tasks communicate through channels:

```plum
plant ch = Channel[int]()

spawn (() => {
    for i in 0..3 { ch.send(i) }
    ch.close()
})()

for n in ch { drop(n) }          # 0 1 2
```

So there are no data races by construction. All the tasks of a program run
on one operating system thread, taking turns; workers (section 12.5,
planned for 3.0) use the other cores.

### 12.4 Taking turns

A task runs until it waits: for a channel, another task, a timer, or
input and output. Then the next task in line runs. `time.sleep(0)` gives
the others a turn. `Channel[T]()` has no limit; `Channel[T](capacity: n)`
makes `send` wait while `n` values wait to be received. `for x in ch`
receives until the channel is closed and empty, and `receive()` gives
`none` then.

Waiting can't happen inside a callback the runtime is running (the
function given to `map` or `filter`, a `to_str`): that is an `Error`.
`time.sleep` inside one simply waits, holding up the other tasks.

The program ends when its own code ends, stopping the tasks still running.
A task's error is thrown by `wait()`; one that nothing waited for is
printed when the program ends, and the exit code is 1. When every task
waits and nothing can wake them, the program ends with a deadlock error.

### 12.5 Workers: every core (planned for 3.0)

Sections 12.5 to 12.11 are the design for multicore Plumlang (issue
[#150](https://github.com/plumlang/plumlang/issues/150), built by
[#133](https://github.com/plumlang/plumlang/issues/133)); why it looks
this way is in [design/concurrency.md](design/concurrency.md).

Tasks take turns on one thread, so they wait well but compute on one
core. To compute on more, a program starts **workers**. A worker is an
**isolate**: its own memory and its own tasks. Isolates share no
variables and no changeable objects, so two workers never change the same
thing at once and nothing needs a lock. Every program starts with one
isolate, the **main isolate**, which runs the program's own code. The
runtime runs the isolates on a pool of threads of the operating system,
one per core (12.5.3), as Erlang does: a worker is not a thread of its
own, so a program can start thousands.

| | Task (`spawn`) | Worker (`workers.start`) |
|---|---|---|
| Runs | taking turns with the other tasks of its isolate | at the same time as other workers, on every core |
| Costs | a few kilobytes; thousands are fine | memory of its own and its own copy of the modules it uses; thousands are fine |
| Shares | channels with the tasks of its isolate | channels, frozen values (12.7) and moved handles (12.6.3), nothing else |
| Good for | waiting: network, files, timers | computing, and keeping state of its own apart |

`spawn` doesn't change: it starts a task on the isolate that calls it. A
worker can spawn tasks of its own, which take turns within the worker as
in 12.4.

```plum
seed workers

branch is_prime(n: int) -> bool {
    if n < 2 { return bad }
    plant d = 2
    while d * d <= n {
        if n % d == 0 { return bad }
        d += 1
    }
    return good
}

branch count_primes(lo: int, hi: int) -> int {
    plant n = 0
    for k in lo..hi {
        if is_prime(k) { n += 1 }
    }
    return n
}

plant halves = [
    workers.start(() => count_primes(0, 5_000_000)),
    workers.start(() => count_primes(5_000_000, 10_000_000)),
]
plant total = 0
for h in halves { total += h.wait() }
drop(total)                    # 664579, in about half the time of one core
```

#### 12.5.1 Starting a worker

Workers come from a standard module, not a keyword: `seed workers`.

| Call | What it does |
|---|---|
| `workers.start(f: () -> T) -> Worker[T]` | start a worker that runs `f()`; the caller keeps going |
| `workers.cores() -> int` | how many cores the machine has, to choose how many workers to start |
| `w.wait() -> T` | the worker's result once it ends; if it failed, its error is thrown here (12.8) |
| `w.done() -> bool` | has the worker ended? |
| `w.stop()` | ask the worker to stop (12.9) |

`f` is the name of a top-level function with no parameters, or a lambda
written in the call (E0702), so the checker can see everything the worker
starts with. Whatever the lambda captures is copied into the worker as it
starts, as if sent through a channel (12.6): each captured value must be
able to cross (E0701), and the lambda can't assign to a captured variable
(E0704), since the worker has its own copy and the change would never
reach the code that started it. Return the result instead, or send it.

```plum
plant rows = load_rows()
plant total = 0.0

workers.start(() => { total = sum(rows) })     # error E0704
plant w = workers.start(() => sum(rows))       # fine: rows is copied in
total = w.wait()
```

`wait()` pauses only the task that waits, as with tasks (12.2): the
isolate's other tasks keep running.

#### 12.5.2 What a worker sees

A worker runs the same program as the main isolate, with memory of its
own:

- The program's functions, trees, traits and kinds are all there.
- The main file's top-level code never runs again in a worker. Its `root`
  constants are there, read from the main isolate's values, which are
  frozen (12.7); one whose line hasn't run yet is the `Error` of 6.1. Its
  module variables (`plant`), and any `root` that holds an unfrozen
  object, are not: code that runs in a worker can't use them (E0703).
  Pass what the worker needs, or make it a `root`. A use the checker can't
  see, through a function value, is a runtime `Error`.
- A module brought in with `seed` is loaded again in each isolate that
  uses it: its top-level code runs once per isolate, and each worker has
  its own copies of its variables.

```plum
seed workers

root SCALE = 3               # fine in a worker: a frozen root
plant calls = 0              # a variable of the main isolate

branch triple(n: int) -> int => n * SCALE

branch triple_and_count(n: int) -> int {
    calls += 1               # error E0703 when this runs in a worker
    return n * SCALE
}

drop(workers.start(() => triple(14)).wait())    # 42
```

#### 12.5.3 How workers are scheduled

The runtime starts one **scheduler thread** per core, `workers.cores()`
of them (`plum run --schedulers n` sets another number, for a container
limited to fewer cores, say). Every isolate, the main isolate included,
runs on one of them:

- An isolate runs on one scheduler thread at a time, so inside an isolate
  nothing changes: its tasks take turns as in 12.4, and its memory is
  never touched by two threads at once. Between turns it may move to
  another scheduler thread; the program can't tell.
- An isolate with a task that can run waits in a queue for a scheduler
  thread. One whose tasks all wait (for a channel, a timer, a socket,
  another worker) takes no thread at all until something wakes it. A
  scheduler thread with nothing to run takes work queued for a busy one.
- **Fairness.** An isolate runs for a budget of **steps**, the calls and
  backward jumps of loops that `max_steps` already counts, and then goes
  to the back of its queue, so a worker in a long loop can't keep the
  others waiting. It is paused only between steps, never inside a call
  into the runtime: a long one (sorting a big list) finishes first, as it
  does before `stop()` (12.9). Pausing an isolate never switches its
  tasks: within an isolate a task still runs until it waits (12.4).
- **Calls that block.** A call that waits on the operating system without
  a way to be told when it is done (looking a host name up, `os.run`,
  reading a file, `ask`) runs on a separate helper thread, not on a
  scheduler thread. Its isolate waits for it, all its tasks with it, as a
  host name lookup waits today, but the other isolates keep every
  scheduler thread.

A program that starts no workers has one isolate, and runs as it did
before workers existed.

#### 12.5.4 Workers in the browser

In the browser build (`make wasm`, [playground.md](playground.md)), each
worker runs in a Web Worker of its own: one of the JavaScript engine's own
threads, with its own instance of `plum.wasm` and its own memory. Isolates
share no memory, so nothing needs `SharedArrayBuffer`, and the page needs
no `Cross-Origin-Opener-Policy` or `-Embedder-Policy` headers. A value
crossing between isolates (12.6) goes through `postMessage`, its bytes in
a transferred buffer; a frozen value is copied once into each Web Worker
that receives it, and keeps its identity (`==` on a frozen object is still
`good` between isolates). `workers.cores()` is what the browser reports
(`navigator.hardwareConcurrency`). Under Node.js, workers use the
`worker_threads` module the same way.

Where Web Workers aren't available (a page whose Content Security Policy
forbids them, a JavaScript engine without them), `workers.start` still
works: the worker runs on the page's one thread, taking turns with the
main isolate as a task does. It is still an isolate, with memory and
module variables of its own, and everything in 12.5 to 12.10 holds, so
the program means the same; it just doesn't run in parallel. The first
`workers.start` writes a note to standard error then: `note: Web Workers
are not available here; workers run one at a time on the page's
thread`.

### 12.6 Sending values between isolates (planned for 3.0)

A `Channel[T]` carries values between isolates as it does between tasks.
A worker gets a channel by capturing it in its start function; both
isolates then hold the same channel, and either can send, receive or
close it. `for x in ch`, capacities and closing work as in 12.4, and
waiting on a channel pauses only the waiting task.

```plum
seed fs
seed workers

plant paths = Channel[str](capacity: 100)
plant sizes = Channel[int]()

plant crew: list[Worker[none]] = []
for _ in 0..workers.cores() {
    crew.push(workers.start(() => {
        for path in paths { sizes.send(len(fs.read(path))) }
    }))
}

for p in ["a.txt", "b.txt", "c.txt"] { paths.send(p) }
paths.close()
for _ in 0..3 { drop(sizes.receive()) }
```

#### 12.6.1 What crosses is a copy

A value sent to another isolate arrives as a copy, as values always do
(11.1): changing it on one side is never seen on the other. The runtime
skips the copying where it can, and which way it goes never changes what
the program means, only how fast it runs:

- **Moved**, when nothing else in the sending isolate holds the value: a
  `send` that is the last use of a variable hands over the memory as it
  is, with no copying, whatever its size.
- **Shared**, when the value is frozen (12.7): only a count goes up.
- **Copied**, otherwise, all the way down.

Handles are the exception: a socket can't be copied, so it always moves
(12.6.3).

```plum
plant rows = build_rows()      # a list of a million floats
out.send(rows)                 # rows isn't used again: moved, nothing copied

plant names = ["Ada", "Grace"]
out_names.send(names)
drop(names)                    # names is used after: the channel gets a copy
```

#### 12.6.2 What can cross

A type that can cross between isolates is **sendable**. The checker
checks it wherever a value enters another isolate: what a start function
captures (and the channels it captures carry), and a worker's result.

| Type | Sendable |
|---|---|
| `int`, `float`, `bool`, `byte`, `none`, `str` | yes |
| `list[T]`, `set[T]`, `map[K, V]`, `T?`, a `kind` | when everything they hold is |
| `Frozen[T]` (12.7) | yes: shared, never copied |
| `Channel[T]` | when `T` is; both isolates then hold the same channel |
| tree and trait objects | no: an object is shared by everyone who holds it (8.3), and two cores changing one object is a data race. Send its values, or freeze it |
| functions and lambdas | no: a function value may capture variables. Send a `kind` that says what to do |
| `Task[T]`, `Worker[T]` | no: they belong to the isolate that started them |
| `Connection`, `Listener`, `UdpSocket` | yes: **moved**, never copied; the sender can't use it after (12.6.3) |
| other handles | no, unless their module's page says they move as in 12.6.3 |

A type that isn't sendable is a compile error where it would cross
(E0701):

```plum
tree Account {
    owner: str
    balance: float = 0.0
}

plant acct = Account("Ada")
workers.start(() => drop(acct.balance))       # error E0701: an Account can't cross
plant balance = acct.balance
workers.start(() => drop(balance))            # fine: a float
```

#### 12.6.3 Moving handles

A handle stands for something of the operating system: a socket, a
listening port. There is one of it, so it can't be copied; it **moves**.
The handles of `net` move: `Connection` (TCP and TLS), `Listener` (TCP
and TLS) and `UdpSocket`. A handle moves when it crosses as any sendable
value does: sent on a channel, captured by a start function, or returned
in a worker's result, alone or inside a list, map or kind.

- After a handle has moved, the receiving isolate owns it, with whatever
  it had read but not yet given out (the rest of a line, a TLS session)
  and its time limits; it closes when the receiver closes it or lets go
  of it.
- The sender can't use it again. The checker reports a use of a variable
  after a `send`, capture or `return` that moved its handle (E0707).
  Where the checker can't see it (another variable, or a list, that holds
  the same handle), the runtime checks: every use of a moved handle in
  the sending isolate throws its module's error for a closed handle
  (`NetError: the connection was moved to another isolate`), and so does
  a task of the sender that was waiting on it when it moved. `close()` on
  a moved handle does nothing, and letting go of it doesn't close what
  the receiver now owns.
- A handle can't be frozen (12.7): it changes as it is used.

A program can accept connections in one isolate and serve them in others:

```plum
seed net
seed workers

plant conns = Channel[Connection](capacity: 64)

for _ in 0..workers.cores() {
    workers.start(() => {
        for conn in conns {
            spawn ((c: Connection) => serve(c))(conn)
        }
    })
}

plant server = net.listen(8080)
while good {
    plant conn = server.accept()
    conns.send(conn)              # conn moves; using it here is E0707
}
```

### 12.7 Frozen values (planned for 3.0)

A **frozen** value never changes again, so every isolate can read it at
the same time without a copy.

`freeze(x)` returns a frozen copy of `x`, all the way down: every list,
map and object inside it is frozen too. When nothing else holds `x` (its
last use) the copy is skipped and `x` itself is frozen. The result has
`x`'s type with every tree or trait type in it wrapped in `Frozen[...]`:
`freeze(Config(...))` is a `Frozen[Config]`, and freezing a
`list[Account]` gives a `list[Frozen[Account]]`.

Freezing a value (a number, string, list, map, set or kind) changes
nothing the program can see, since values never change under their holder
anyway (11.1); it only makes sending the value free. A list made from a
frozen one, with `push` say, is a new list and isn't frozen.

A `Frozen[T]` object reads like a `T`: its fields, the methods that don't
change it, printing, `==` and `is`. Changing it is a compile error
(E0706): assigning a field, or calling a method that assigns one, itself
or through another method. A function that takes a `T` can't be given a
`Frozen[T]`, since it might change it; write the parameter as
`Frozen[T]`.

```plum
tree Config {
    host: str
    port: int
    branch url() -> str => "http://{self.host}:{self.port}"
    branch move_to(port: int) { self.port = port }
}

plant config = freeze(Config("localhost", 8080))   # a Frozen[Config]
drop(config.url())          # http://localhost:8080
config.port = 9090          # error E0706: config is frozen
config.move_to(9090)        # error E0706: move_to changes the object
```

A frozen object sent to workers is the same object in every isolate:
nothing is copied, and `==` on a tree without `equals` (identity) is
`good` between them.

```plum
seed workers

root SETTINGS = freeze(Config("db.local", 5432))

branch connect_all() -> str => SETTINGS.url()    # fine: a frozen root

plant w = workers.start(connect_all)
drop(w.wait())              # http://db.local:5432
```

Some values are frozen without asking: literals and the program's own
code, and the value of every `root` constant that isn't an unfrozen
object. A `root` that holds an object is frozen only by writing
`freeze`, as above, so existing programs keep their meaning.

Some things can't be frozen (E0705), because changing is what they are
for: functions, tasks, workers, channels, sockets and other handles, and
objects of a tree with a `weak` field anywhere inside. A `weak` field
can't hold a `Frozen` object either (E0425): `Frozen[T]` is not a tree
type.

### 12.8 Errors in workers (planned for 3.0)

An error a worker doesn't catch ends the worker, and `w.wait()` throws it
in the isolate that waits, with the trace of where it happened in the
worker, as `task.wait()` does (12.1).

The error, and a worker's result, are **handed over** whole: the worker
has ended, so nothing in it can use them again, and they arrive as the
same trees with the same fields, objects included. This is why a worker's
result type may hold tree objects though a channel's can't; it still
can't hold functions, tasks or workers (E0701), and handles in it move
(12.6.3). An error with a field that can't be handed over (it holds a
function, task or worker) arrives as a `WorkerError` instead, whose message names the error's tree
and repeats its message.

```plum
seed workers

tree BadRow : Error {
    line: int
}

branch check(rows: list[str]) -> int {
    for i, row in rows.enumerate() {
        if row == "" { throw BadRow("an empty row", i + 1) }
    }
    return len(rows)
}

plant rows = ["a", "", "c"]
plant w = workers.start(() => check(rows))
try {
    drop(w.wait())
} catch e: BadRow {
    drop("line {e.line}: {e.message}")       # line 2: an empty row
}
```

A worker's error that nothing waited for is printed when the program
ends, with the line that started the worker, and the exit code is 1, as for
tasks (12.4). Inside a worker, its own tasks follow 12.4: their errors
reach the worker's code through `wait()`.

### 12.9 Stopping, and the end of a program (planned for 3.0)

- A worker ends when its start function returns or throws. Tasks it
  spawned that are still running are stopped then, as at the end of a
  program (12.4).
- `w.stop()` asks a worker to stop. At its next call, loop turn or wait,
  the worker unwinds: `finally` blocks run, no `catch` catches it, and its
  tasks stop. `w.wait()` then throws a `StoppedError`. Stopping a worker
  that has ended does nothing. A long call into the runtime (sorting a big
  list, say) finishes before the worker stops.
- Dropping the last reference to a `Worker` doesn't stop it, as with
  tasks.
- Ending a worker doesn't close its channels: close a channel when you
  are done sending, and receivers in other isolates see the end.
- The program ends when the main isolate's own code ends; workers still
  running are stopped, as tasks are. `exit(code)` anywhere, in a worker
  too, ends the whole program at once.
- When every task of every isolate waits and nothing can wake any of
  them, the program ends with a deadlock error, as in 12.4.

```plum
seed workers
seed time

plant w = workers.start(() => {
    while good { crunch_next() }
})
time.sleep(5.0)
w.stop()
try {
    w.wait()
} catch e: StoppedError {
    drop("stopped after 5 seconds")
}
```

### 12.10 Memory in isolates (planned for 3.0)

Each isolate counts the references to its own values as today, without
the atomic instructions that cores need to share a count; values freed in
one isolate never wait for another. Only frozen values, which isolates
share, have their counts changed atomically. A program that starts no
workers runs as fast as it did before workers existed.

Memory is still freed the moment nothing refers to it (11.2), and a cycle
of objects is still a leak inside its isolate, broken with `weak` (11.3).
`--check-leaks` and `--mem-stats` cover every isolate, naming the worker
a leaked object belongs to. Frozen objects that form a cycle are never
freed; `--check-leaks` reports them too.

`plum run --collect-cycles` turns on a **cycle collector** in each
isolate: from time to time it finds the objects of that isolate kept
alive only by cycles among themselves, and frees them (their sockets and
files close). It is off by default, since it costs time and its pauses,
though short, are not zero. It looks only at its own isolate: never at
frozen values, never at another isolate, so one worker's collection never
stops another. `weak` stays the way to write code that doesn't leak.

```plum
tree Peer {
    name: str
    other: Peer? = none          # strong both ways: a cycle
}

plant a = Peer("a")
plant b = Peer("b")
a.other = b
b.other = a
# Without --collect-cycles, a and b leak when the program lets go of them;
# with it, the collector frees both. `other: weak Peer? = none` needs neither.
```

### 12.11 net.http on every core (planned for 3.0)

`http.serve(make_app, port = 8080, host = "0.0.0.0", workers = 0)` serves
HTTP from several isolates at once: `workers` of them, or one per
scheduler thread (12.5.3) when it is 0. Each isolate calls `make_app()` to
build its own server (routes, middleware and all). Inside an isolate, each
connection is served by a task of its own, as with `app.listen`.

How connections reach the isolates depends on what the operating system
can do well:

- **Linux** (and FreeBSD, with `SO_REUSEPORT_LB`): every isolate opens
  its own listener on the same port with `SO_REUSEPORT`, and the kernel
  spreads new connections over them. Nothing is handed between isolates,
  and accepting scales with the cores.
- **Everywhere else** (macOS and the other BSDs, Windows, where a shared
  port either isn't allowed or doesn't spread connections): one small
  isolate of `http.serve`'s own accepts, and moves each connection
  (12.6.3) to the serving isolate with the fewest open connections.

Which one is used never changes what a handler sees.

`make_app` follows the rules of a start function (12.5.1): a top-level
function, or a lambda written in the call whose captures can cross.
`app.listen` keeps serving from one isolate.

```plum
seed net.http

branch make_app() -> http.Server {
    plant app = http.server()
    app.get("/hello/:name", (req) => http.text("Hello, {req.params["name"]}!"))
    return app
}

http.serve(make_app, port: 8080)          # one isolate per core
```

Handlers on different isolates share no variables: a counter in a module
counts the requests of its own isolate only. Keep state the isolates share
in a database, or in one worker the others reach through channels,
captured by `make_app`:

```plum
seed net.http
seed workers

plant hits = Channel[str](capacity: 1000)

workers.start(() => {
    plant counts: map[str, int] = {:}
    for path in hits { counts[path] = (counts.get(path) ?? 0) + 1 }
})

http.serve(() => {
    plant app = http.server()
    app.use((req, next) => {
        hits.send(req.path)
        return next(req)
    })
    app.get("/", (req) => http.text("hi"))
    return app
}, port: 8080)
```

## 13. The standard library

Modules are imported with `seed`. The 1.0 library is planned as:

| Module | Contents |
|---|---|
| `math` | constants and functions on numbers, random numbers |
| `string` | the methods of `str` (always available on strings) |
| `list`, `map`, `set` | the methods of collections (always available) |
| `io` | standard input and output, formatting |
| `fs` | files and directories |
| `os` | arguments, environment, processes, exit |
| `time` | clocks, dates, durations, `sleep` |
| `json` | parse and produce JSON, mapped to maps, lists and trees |
| `net` | TCP and UDP sockets, TLS, DNS |
| `net.http` | an HTTP and HTTPS server and client |
| `hash` | cryptographic digests |
| `path` | file paths as text: join them, take them apart |
| `base64`, `hex` | bytes as Base64 or hexadecimal text, and back |
| `utf8` | code points; checking and repairing UTF-8 |
| `csv` | read and write comma-separated values |
| `uuid` | unique identifiers (RFC 9562): random v4, time-ordered v7 |
| `semver` | semantic versions and version ranges |
| `url` | parse, build and resolve URLs; percent-encoding and query strings |
| `toml` | read and write TOML 1.0, mapped to maps and lists |
| `test` | assertions and a test runner (`plum test`) |

Each module is documented in `docs/stdlib/` as it lands, and `plum stdlib`
lists the functions of every module with their signatures. The methods of
`str` are documented as the `str` page there. A web server, for
a taste:

```plum
seed net.http

plant app = http.server()

app.get("/hello/:name", (req) => http.text("Hello, {req.params["name"]}!"))

app.listen(8080)
```

Each connection runs in its own task (section 12), and `http.serve` will
run a server on every core (section 12.11, planned for 3.0).
`docs/stdlib/net.http.md`
describes the server (routes with `:params`, middleware, static files) and
the client (`http.get`, `http.post_json`, ...). Some standard modules, like
`net.http`, are written in Plumlang and built into `plum`.

## 14. Running programs

```
plum run program.plum [args...]     # run a program
plum run --check-leaks program.plum # ...and report objects left in cycles (11.3)
plum run --mem-stats program.plum   # ...and print the memory it used (11.5)
plum program.plum [args...]         # the same
plum check program.plum             # check for errors without running
plum test [paths...]                # run the tests (14.3)
plum fmt files-or-directories...    # format source files in place (14.1)
plum fmt --check paths...           # ...change nothing; list files that would change
plum fmt --stdin                    # format standard input to standard output
plum                                # interactive prompt, on a terminal (14.2)
plum repl                           # the interactive prompt, also with piped input
plum lsp                            # the language server, for editors (docs/lsp.md)
```

Exit codes: 0 success, 1 the program failed (compile or runtime error, or
`exit(1)`), 2 the command was used wrongly, 3 a source file could not be
read.

### 14.1 Formatting

`plum fmt` gives source code one canonical layout. It formats the files it
is given, and every `.plum` file under the directories it is given;
`--check` changes nothing, lists the files that would change and exits 1 if
there are any; `--stdout` writes the result to standard output instead of
the file. `--stdin` (or `-`) reads the source from standard input and writes
the formatted source to standard output, for editors that pipe a buffer
through a formatter. A file with syntax errors is reported and left as it
is (exit 1); with `--stdin` the errors go to standard error and nothing is
written to standard output.

The layout:

- four spaces per level; `{` ends its line and `}` starts its own;
- one space around binary operators and after commas, none around `..`;
- at most one blank line between statements, none at the start or end of a
  block, and one before and after a top-level `branch`, `tree`, `trait` or
  `kind` that takes more than one line;
- a list, map, call, parameter list or kind that the source spreads over
  several lines gets one item per line, with a trailing comma (kind cases
  go without); one on a single line stays on one line. An `if` expression
  over several lines puts each branch on lines of its own;
- literals keep their spelling (`0xFF`, `1_000`, escapes, `"""` strings,
  `true`), and parentheses the source wrote are kept;
- a comment on a line of its own stays on a line of its own, before what
  follows it; one after code stays after that code, and trailing comments
  on neighbouring lines are lined up. A block comment keeps its text as
  written (only its first line is indented); one inside an expression goes
  after the code of its line. `##text` gets a space: `## text`.

Formatting never changes what a program means, and formatting a formatted
file changes nothing.

### 14.2 The interactive prompt

`plum` with no arguments, when standard input is a terminal, starts a REPL;
with piped input it prints the help instead (`plum repl` reads piped input
as entries, without prompts). Each entry, a line, or several lines while a
bracket is open, a line ends with an operator, `,` or `=>`, or a `"""`
string is open, is checked and run at once, as if it were the next part of
one program:

- What an entry declares (variables, functions, trees, traits, kinds and
  `seed` imports) is there for the entries after it. An entry may declare
  a name again; the new declaration hides the old one from then on, while
  code written before keeps the old one.
- An entry that is one expression prints its value as `drop` would (an
  object through its `to_str`) and its type: `= 42 : int`. A `none` value
  prints nothing. Such an expression is not an unused value (5.7).
- An entry with a compile error or a runtime error reports it and leaves
  nothing it declared behind; what earlier entries made is kept. Imported
  files are loaded and run once, by the first entry that imports them.
- Tasks run while an entry runs, and the entry ends when its own code
  does; tasks still running carry on while later entries run (12).
- `exit(n)` ends the session with exit code `n`; otherwise it ends with 0
  at the end of the input, `:quit` or Ctrl-D.

Commands start with `:`: `:help`, `:type expr` (the expression's type,
without running it), `:load file.plum` (run a file's code as an entry),
`:reset` (forget every declaration) and `:quit` (or `:q`).

### 14.3 Testing

`plum test` runs a project's tests. A **test file** is a file whose name
ends in `_test.plum`; a **test** is a top-level function in it whose name
starts with `test_`, with no parameters and no result. No new syntax is
involved: a test passes when it returns, and fails when an error escapes
it, usually the `AssertError` of a failed `assert` (section 7).

```plum
# math_test.plum
seed test
seed ./math_utils { average }

branch test_average() {
    assert(average([2, 4, 6]) == 4)
}

branch test_average_of_nothing() {
    plant e = test.throws(() => {
        average([])
    })
    assert(e is ZeroDivisionError)
}
```

```
plum test                       # every *_test.plum under the current directory
plum test tests src/x_test.plum # those under a directory, and one file by name
plum test --filter average      # only the tests whose name contains "average"
plum test --json                # the report as JSON, for editors and CI
```

- **Finding tests.** Directories are searched for test files, in name
  order, skipping hidden entries; a file named on the command line is a
  test file whatever its name. Tests run in the order they are written.
  A `test_` function that takes parameters or returns a value is reported
  as a failing test.
- **Each test on its own.** Every test runs in a fresh program: the
  test file's modules are loaded again, their top-level code runs, then
  the test function. Module variables, objects and tasks from one test
  never reach another, and a failing test doesn't stop the rest. What
  outlives the program is shared: files, the environment, the network.
- **Errors in the file.** A test file with compile errors is reported
  (on standard error, as by `plum check`) and none of its tests run; it
  counts as a failure. Warnings are reported and the tests run.
- **The report.** For each file, each test passes (`ok`) or fails
  (`FAIL`). A failure shows the error, the file and line where it was
  thrown (for an error thrown inside the standard library, the line of the
  test's code that called it), the calls that led there, and what the test
  printed; what a passing test prints is not shown. A last line counts the
  passes and failures. `--json` writes instead one JSON object with
  `tests` (each with `file`, `name`, `line`, `passed`, and for a failure
  `error`: `type`, `message`, `file`, `line`, `trace`, and `output`),
  `broken` (test files with errors), `passed` and `failed`.
- **Exit code.** 0 when every test passed, also when there were none; 1
  when a test failed or a test file has errors; 2 for wrong usage; 3 when
  a path can't be read.
- **Expected errors.** `test.throws(f)` (`seed test`) runs `f` and
  returns the error it threw, for `is` to check its tree; if `f` throws
  nothing, the test fails with an `AssertError`.

## 15. Changes from the Python prototype

The Python prototype (plum-lang v0.1 to v0.10) introduced the garden
keywords; Plumlang keeps them and changes the rest where the prototype was
hard to read or unsafe:

| Prototype | Plumlang | Why |
|---|---|---|
| `plant int x = 5` | `plant x = 5`, `plant x: int = 5` | Inference; the type follows the name everywhere |
| `branch int add(int a, int b)` | `branch add(a: int, b: int) -> int` | The same `name: type` order as variables |
| `drop x` and `drop(x)` | `drop(x)` only | One way; `drop` is an ordinary function |
| `&&` `\|\|` `!` | `and` `or` `not` | Reads as English, and frees `!` |
| `++` `--` | `x += 1` | One way to add one; no prefix/postfix puzzles |
| C-style `for (init; cond; step)` | `for i in 0..n`, `while` | Fewer ways to write a loop, no off-by-one setup |
| `if (cond)` | `if cond` | Parentheses optional, like any expression |
| Every number a float (`25.0`) | `int` and `float` are separate | `drop(25)` prints `25` |
| `10 / 2` gives `5.0` | `/` gives a float, `//` an int | Kept, and made explicit |
| Mixed `#`, `//`, `--`, `/* */` comments | `#`, `#[ ]#` and `##` | One comment character: lines, blocks and docs |
| Type errors while running | Type errors before running | Caught early, with a location |
| Python exceptions leak out | Plumlang errors with a stack trace | Messages about your program, not the interpreter |

## 16. Open questions

To be settled before the parser (#10) depends on them:

1. Should `trait` have a garden-themed name, like the other declaration
   keywords? The current choice is `trait`, the term most programmers
   already know.
2. `true`/`false` as alternative spellings of `good`/`bad` (section 2.5):
   keep both, or allow only one?
3. Generic trees and functions (`tree Stack[T]`) are left out of 1.0 except
   for the built-in collections. Are they needed for 1.0?
