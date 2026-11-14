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

Functions can be called before their declaration in the same module.

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
plant sorted = people.sort_by((p) => p.age)
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
other value in Plumlang is copied (section 11). Two names refer to the same
object when `a is b`.

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
  `equals`. Without it, `==` asks whether both are the same object.
  (Comparisons inside collections, like `list.contains`, still compare
  objects by identity.)

`Order` (`compare`, for `<` and sorting), `Hash` (`hash`, for map keys)
and `Iterable` (`iter`, for `for`) come with the standard library.

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
program needs no `main` function.

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
program, so `plum run my-project` runs it. Third-party packages are a
post-1.0 feature.

```toml
[package]
name = "weather-api"
version = "0.1.0"
entry = "main.plum"
```

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
| `ValueError` | `byte(n)` out of 0 to 255; an int power with a negative exponent; a shift count out of 0 to 63; a format that doesn't fit the value |
| `AssertError` | `assert(condition)` when the condition is `bad` |
| `Error` | too much recursion |

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
they are freed. A program can run out of memory, which ends it with a
runtime error.

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
given, and tree objects cannot be passed to a task (a compile error) unless
the tree is marked `shared`, which allows only fields that are themselves
values and makes each method call atomic. Tasks communicate through
channels:

```plum
plant ch = Channel[int]()

spawn (() => {
    for i in 0..3 { ch.send(i) }
    ch.close()
})()

for n in ch { drop(n) }          # 0 1 2
```

So there are no data races by construction. All tasks run on one operating
system thread in 1.0; using several cores is planned after 1.0.

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
| `net` | TCP and UDP sockets, DNS |
| `net.http` | an HTTP server and client |
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

Each request runs in its own task (section 12).

## 14. Running programs

```
plum run program.plum [args...]     # run a program
plum program.plum [args...]         # the same
plum check program.plum             # check for errors without running
plum test                           # run tests (Stage 5)
plum fmt                            # format source files (Stage 6)
plum                                # interactive prompt (Stage 6)
```

Exit codes: 0 success, 1 the program failed (compile or runtime error, or
`exit(1)`), 2 the command was used wrongly, 3 a source file could not be
read.

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
| Mixed `#`, `//`, `--` comments | `#` only | One comment syntax |
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
