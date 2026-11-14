# Plumlang error codes

Every compile error has a code, shown as `error[E0201]`. This page explains
each one, with an example that causes it and the fix.

| Range | Stage |
|---|---|
| E01xx | Reading the source text (lexer) |
| E02xx | Syntax (parser) |
| E03xx | Names |
| E04xx | Types |
| E05xx | Control flow |
| W05xx | Control-flow warnings (the program still runs) |
| E06xx | Compiling to bytecode |

Codes never change meaning. A retired code is marked retired, not reused.
`make test` checks that every code in the compiler is documented here and
that every code documented here is still used.

## Characters

### E0101

The file contains bytes that are not valid UTF-8. Plumlang source files must
be UTF-8; save the file with UTF-8 encoding in your editor.

### E0102

A character that has no meaning in Plumlang, such as `@` or `$`, outside a
string or comment.

```plum
plant price = $5      # error
plant price = 5       # fix
```

### E0103

An operator from another language. Plumlang spells logic with words and has
no increment operators.

```plum
if a && !b { }        # error
if a and not b { }    # fix

count++               # error
count += 1            # fix
```

### E0104

A semicolon. A statement ends at the end of its line.

```plum
plant x = 1;          # error
plant x = 1           # fix
```

### E0105

A single-quoted string. Strings use double quotes.

```plum
plant name = 'Ada'    # error
plant name = "Ada"    # fix
```

## Numbers

### E0110

A `_` that is not between two digits. `_` only separates digits.

```plum
plant n = 1_          # error
plant n = 1__000      # error
plant n = 1_000       # fix
```

### E0111

A digit that does not exist in the number's base.

```plum
plant b = 0b102       # error: binary has only 0 and 1
plant b = 0b101       # fix
```

### E0112

A letter straight after a number.

```plum
plant n = 12abc       # error
plant n = 12          # fix
```

### E0113

`0x`, `0o` or `0b` with no digits after it.

```plum
plant mask = 0x       # error
plant mask = 0xFF     # fix
```

### E0114

An exponent with no digits.

```plum
plant big = 1e        # error
plant big = 1e9       # fix
```

### E0115

A number that starts with `0`. In some languages that means octal, so
Plumlang asks you to say which you meant.

```plum
plant mode = 0755     # error
plant mode = 755      # fix, decimal
plant mode = 0o755    # fix, octal
```

### E0116

An integer larger than the largest `int`, 9223372036854775807.

### E0117

A float too large to represent, such as `1e999`.

## Strings

### E0120

A string with no closing quote. A `"` string must end on the line it starts
on; use `"""` for text over several lines.

```plum
drop("Hello)          # error
drop("Hello")         # fix
```

### E0121

An escape that doesn't exist. The escapes are `\n` `\t` `\r` `\0` `\\` `\"`
`\{` `\}` and `\u{...}`.

```plum
drop("a\qb")          # error
drop("a\\qb")         # fix: a literal backslash
```

### E0122

`\u` without braces.

```plum
drop("A")        # error
drop("\u{41}")        # fix
```

### E0123

`\u{...}` with no digits, a non-hexadecimal digit, or more than six digits.

### E0124

`\u{...}` with a number that is not a Unicode character: above `10FFFF`, or a
surrogate (`D800` to `DFFF`).

### E0125

A `}` in a string. Braces start and end interpolation, so a literal one is
written `\}`.

```plum
drop("a } b")         # error
drop("a \} b")        # fix
```

### E0126

In a `"""` string, a line indented less than the closing `"""`. That
indentation is removed from every line, so no line may have less.

```plum
plant s = """
    first
  second              # error: indented less than the closing """
    """
```

### E0127

An interpolation with `:` but no format after it.

```plum
drop("{price:}")      # error
drop("{price:.2}")    # fix
```

## Brackets

### E0130

A `(`, `[` or `{` that is never closed. When the next line starts a new
statement, the error points at the open bracket.

```plum
drop((1 + 2)          # error: the first ( is never closed
drop((1 + 2))         # fix
```

### E0131

A closing bracket of the wrong kind.

```plum
plant xs = [1, 2)     # error
plant xs = [1, 2]     # fix
```

### E0132

A closing bracket with nothing to close.

```plum
plant y = 3]          # error
plant y = 3           # fix
```

## Syntax

### E0201

Something other than what the grammar allows here. The message says what was
expected and what was found.

```plum
f(x, , y)             # error: expected an expression, found ','
```

### E0202

Chained comparisons. `a < b < c` does not mean what it does in maths.

```plum
if 0 < x < 10 { }             # error
if 0 < x and x < 10 { }       # fix
```

### E0203

Chained ranges, such as `0..1..2`.

### E0204

`{}` in a value. It could be an empty block or an empty map, so the empty map
is written `{:}`.

```plum
plant ages: map[str, int] = {}     # error
plant ages: map[str, int] = {:}    # fix
```

### E0205

An `if` used as a value without `else`. A value needs a result either way.

```plum
plant label = if n == 1 { "item" }                  # error
plant label = if n == 1 { "item" } else { "items" } # fix
```

### E0206

`spawn` with something other than a function call.

```plum
spawn worker          # error
spawn worker()        # fix
```

### E0207

A positional argument after a named one.

```plum
connect(secure: good, "example.com")    # error
connect("example.com", secure: good)    # fix
```

### E0208

A type made optional twice, such as `int??`. One `?` is enough.

### E0209

An assignment whose left side cannot be assigned: only a variable, a field
or a list or map element can.

```plum
5 = x                 # error
f() += 1              # error
a?.b = 3              # error: assign through a known object instead
```

### E0210

A `plant` or `root` without a value.

```plum
plant count           # error
plant count = 0       # fix
```

### E0211

A parameter without a default after one with a default.

```plum
branch f(a: int = 1, b: int) { }    # error
branch f(b: int, a: int = 1) { }    # fix
```

### E0212

A `try` block with neither `catch` nor `finally`.

### E0213

A field written with `plant` or `root` inside a tree.

```plum
tree Point {
    plant x = 0       # error
    x: int = 0        # fix
}
```

### E0214

A `kind` with no cases.

### E0215

A `seed` after other statements. Imports go at the top of the file.

### E0216

A `{` at the start of a statement. Plumlang has no free-standing blocks;
for an empty map write `plant m = {:}`.

### E0217

A keyword used as a field's name, like `kind: Kind` or `match: str`.
Keywords can't name anything; pick another name (`kind_`, `shape_kind`).

## Names

### E0301

A name that isn't declared anywhere visible. If it is close to a name that
exists, the message suggests it; names from other languages get the
Plumlang word.

```plum
plant score = 10
drop(scroe)           # error: did you mean 'score'?
print("hi")           # error: Plumlang prints with drop(...)
```

### E0302

The same name declared twice in one scope. An inner block may reuse an outer
name, but a function's body shares its parameters' scope.

```plum
branch greet(name: str) {
    plant name = "x"  # error: already declared, as a parameter
}
```

### E0303

A module variable used before the line that declares it. Functions, trees,
traits and kinds can be used anywhere in the file; variables only after
their `plant` or `root` line (inside a function body, any module variable is
visible, since the body runs later).

```plum
drop(total)           # error
plant total = 5
```

### E0304

A change to something that can't change: a `root` constant, a parameter, a
loop variable, a function or a type.

```plum
root MAX = 100
MAX = 200             # error: declare it with 'plant' to change it
```

### E0305

A type that doesn't exist, or a name used as a type that isn't one.

```plum
plant s: Strng = ""   # error: did you mean 'str'?
plant b: boolean = good   # error: in Plumlang it is 'bool'
```

### E0306

`self` or `super` outside a method of a tree.

### E0307

`return` outside a function.

### E0308

A `seed` of a module that doesn't exist: a standard module name that
isn't one (the message lists them; `plum stdlib` shows their functions),
or a file that can't be read (`seed ./shapes` looks for `shapes.plum` next
to the importing file).

### E0309

Something in a `match` pattern that can't be one. A pattern is a literal, a
range, a kind case (with names to bind its values), a `root` constant, or
`_`. A bare name doesn't match anything: use `else`.

```plum
match light {
    Red => stop()
    other => go()     # error
    else => go()      # fix
}
```

### E0310

A `tree`, `trait` or `kind` declared inside a function. Declare it at the
top of the file.

### E0311

Modules that import each other in a circle: `a` imports `b`, which
imports `a` (perhaps through others). The message shows the whole circle.
Move what both need into a module of its own that both import.

## Types

### E0401

A value that is computed and then thrown away. A line on its own must do
something: call a function, assign, declare.

```plum
total + 1             # error: the sum goes nowhere
total += 1            # fix
x == 5                # error: did you mean '='?
```

### E0402

A value of the wrong type. The only automatic conversion is int to float;
everything else is written: `int(x)`, `float(x)`, `str(x)`.

```plum
plant count: int = "5"      # error
plant count: int = int("5") # fix
plant n = 5
n /= 2                      # error: '/=' makes a float; use '//='
```

### E0403

An operator used on values it doesn't work on: adding text and a number,
comparing values that are never equal, bitwise operators on floats.

```plum
drop("total: " + 5)         # error
drop("total: {5}")          # fix: interpolation
```

### E0404

A condition that isn't a bool. Plumlang has no "truthiness": write the
comparison you mean.

```plum
if count { }                # error
if count > 0 { }            # fix
```

### E0405

A call of something that isn't a function: a number, a trait, a kind.

### E0406

The wrong number of arguments, or a required argument left out.

```plum
branch add(a: int, b: int) -> int => a + b
add(1)                      # error: missing 'b'
```

### E0407

A named argument the function doesn't have, or an argument given twice.

### E0408

A value that may be `none` used as if it were always there. Give a default
with `??`, call through it with `?.`, or check it with `if plant`.

```plum
plant nickname: str? = ask("Nickname? ")
drop(nickname.upper())              # error
drop(nickname?.upper() ?? "none")   # fix
if plant n = nickname { drop(n) }   # fix
```

### E0409

A type the checker can't work out: `none`, `[]` or `{:}` with nothing to
say what they hold. Write the type.

```plum
plant names = []                    # error
plant names: list[str] = []         # fix
```

### E0410

Indexing with the wrong type of index, or indexing something that can't be
indexed.

### E0411

A `return` that doesn't fit its function: a value from a function with no
`->` result, or no value from one that has it.

### E0412

A `for` loop over something that isn't a range, list, set, map, string or
channel; or two loop names over something other than a map.

```plum
for i, x in items { }               # error
for i, x in items.enumerate() { }   # fix
```

### E0413

Throwing or catching something that isn't an error. Errors are trees that
extend `Error` (or one of the built-in error trees).

```plum
throw "failed"                      # error
throw ValueError("failed")          # fix
```

### E0414

A type with the wrong number of type arguments: `list` needs one
(`list[int]`), `map` two (`map[str, int]`), `int` none.

### E0418

A range (`0..5`) used as a value. A range is written where it is used: in
a `for` loop, a slice, after `in`, or as a `match` pattern.

```plum
plant r = 0..5        # error
for i in 0..5 { }     # fix
```

### E0415

A field, method or kind case that doesn't exist.

### E0416

A conversion that isn't allowed: `bool(x)` (write the comparison), `as`
between non-numbers (use `int(...)`, `str(...)`), or converting a value
that can't be converted.

### E0417

A `match` pattern that can't match the value: a case of another kind, a
literal of another type, or a case pattern naming the wrong number of
values.

### E0420

A tree names a trait but doesn't have one of the trait's methods (and the
trait gives no default). Add the method with the trait's parameters and
result.

### E0421

A method that overrides a parent's method, or provides a trait's, with
different parameters or a different result. Code that calls it through
the parent or the trait passes what that declaration says, so the types
must be the same.

```plum
tree Animal { branch speak() -> str => "..." }
tree Dog : Animal { branch speak(loud: bool) -> str => "woof" }   # E0421
```

### E0422

A field without a default that nothing sets. A tree with `init` must set
each such field at the start of `init` (`self.name = name`), before
anything else uses `self`; a tree that adds such a field needs its own
`init` if its parent has one.

```plum
tree Point {
    x: float
    y: float
    branch init(r: float) {
        self.x = r
        drop(self)        # E0422: self.y isn't set yet
    }
}
```

### E0423

A field or method whose name starts with `_` used outside its tree's
methods (or those of trees extending it). The underscore makes it private.

### E0424

A method the language calls itself, written with the wrong shape:
`to_str() -> str` (for printing), `equals(other: <this tree>) -> bool`
(for `==`), and `init(...)` with no result.

### E0425

`weak` on a field whose type isn't an optional tree or trait. A weak
reference reads as `none` once its object is freed, so its type must be
optional, like `parent: weak Node? = none`; numbers, strings and
collections are values and can't be weak.

## Control flow

### E0501

A function with a result (`-> type`) that can finish without returning
one. Every path must end in `return` (or `throw`).

```plum
branch sign(n: int) -> str {
    if n > 0 { return "positive" }
    if n < 0 { return "negative" }
}                     # error: no return when n is 0
```

Add `return "zero"` at the end, or make the last `if` an `if ... else`.

### E0502

`break` or `continue` outside a loop. A lambda is a new function, so a
`break` inside one doesn't reach a loop around it.

### E0503

A `match` that doesn't cover every possible value (spec 5.3): a kind case
left out, a bool match without both `good` and `bad`, or a match on a number
or string without an `else` arm.

```plum
kind Light { Red, Yellow, Green }
match light {
    Red => stop()
    Green => go()
}                     # error: doesn't cover 'Yellow'
```

## Warnings

### W0501

Code after a `return`, `throw`, `break` or `continue` that can never run.

### W0502

A variable that is declared but never used. Remove it, or start its name
with `_` to say it is unused on purpose.

## Compiling

### E0601

A feature the compiler can't turn into bytecode yet. The program is
correct (`plum check` accepts it), but it can't run until the roadmap item
named in the message: today that is tree methods, `init`, kinds and
`match` (#22), modules (#23) and tasks (#27).

### E0602

A limit of the bytecode format: a function with more than 65,535 constants
or local variables, or a block or loop too long to jump over. Split the
function into smaller ones.
