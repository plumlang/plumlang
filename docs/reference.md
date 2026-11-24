# Plumlang reference

The whole language on one page, for people who know another language and
for coding agents. The [specification](spec.md) is the full contract; this
page is checked by `make test`: every `plum` block passes `plum check`, and
every `output` block is exactly what the block before it prints.

## Rules that differ from other languages

- Print with `drop(...)`, always with parentheses. There is no `print`.
- Variables are `plant`, constants `root`, functions `branch`, classes
  `tree`, imports `seed`. Booleans are `good` / `bad` (`true` / `false` also
  work). The absent value is `none`.
- No semicolons; a statement ends at the end of its line. Comments start with
  `#`; block comments are `#[ ... ]#` and nest; `##` lines before a
  declaration are its doc comment (Markdown).
- Types follow names: `x: int`, `branch f(a: int) -> str`. Parameter types
  and non-`none` results are always written; local types are inferred.
- No truthiness: conditions must be `bool` (`if n > 0`, not `if n`).
- `/` always gives a `float`; `//` is integer division. Integer overflow is
  an error, never wrap-around.
- `and`, `or`, `not`: there is no `&&`, `||`, `!`, `++` or `--`.
- Only `T?` types can be `none`. Use `??`, `?.` or `if plant x = opt`.
- Strings, lists, maps and sets are values: assigning copies them. Tree
  objects are shared references.
- An empty list or map needs a type: `plant xs: list[int] = []`,
  `plant m: map[str, int] = {:}`.

## A complete program

```plum
seed math

root GREETING = "Hello"

tree Planet {
    name: str
    radius_km: float

    branch area() -> float => 4.0 * math.PI * self.radius_km ** 2
}

plant planets = [Planet("Mercury", 2439.7), Planet("Earth", 6371.0)]
for p in planets {
    drop("{GREETING}, {p.name}! {p.area():.0} km²")
}
```

```output
Hello, Mercury! 74796748 km²
Hello, Earth! 510064472 km²
```

## Values and types

| Type | Literals |
|---|---|
| `int` (64-bit) | `42`, `1_000_000`, `0xFF`, `0o755`, `0b1010` |
| `float` (64-bit) | `3.14`, `1e9`, `2.5e-3` (not `.5` or `5.`) |
| `bool` | `good`, `bad` |
| `str` (UTF-8, immutable) | `"text"`, `"tab\t, unicode \u{1F351}"`, `"""multi-line"""` |
| `byte` | `byte(255)` |
| `none` | `none` |
| `list[T]` | `[1, 2, 3]` |
| `map[K, V]` | `{"a": 1}`, empty `{:}` |
| `set[T]` | `set([1, 2])` |
| `T?` | a `T` or `none` |
| `(int, int) -> int` | a function |

```plum
plant count = 3                    # int, inferred
plant ratio: float = 1             # an int becomes a float where a float is expected
plant name: str? = none
plant total = 7 / 2                # 3.5: / always gives a float
plant whole = 7 // 2               # 3
plant text = str(42) + " " + str(1.5)
plant parsed = int("42")           # throws ParseError on bad input
plant safe = try int("x") ?? 0     # try as an expression: an error becomes none
drop(count, ratio, name ?? "anon", total, whole, text, parsed, safe, type_of(ratio))
```

```output
3 1.0 anon 3.5 3 42 1.5 42 0 float
```

## Strings

```plum
plant name = "Ada"
plant price = 3.14159
drop("Hello, {name}! {2 + 2} {price:.2} [{name:>6}] [{name:<6}] {255:x}")
drop(name.upper(), name.lower(), len(name), name[0], name[1..3])
drop("a,b,c".split(","), "  hi  ".trim(), "plum".replace("p", "P"))
drop("plum".starts_with("pl"), "plum".contains("lu"), "plum".find("u") ?? -1)
plant poem = """
    Roses are red,
    Plums are purple.
    """
drop(poem.lines())
```

```output
Hello, Ada! 4 3.14 [   Ada] [Ada   ] ff
ADA ada 3 A da
["a", "b", "c"] hi Plum
good good 2
["Roses are red,", "Plums are purple."]
```

String methods: `upper lower trim trim_start trim_end split lines chars
contains starts_with ends_with find count replace repeat reverse pad_start
pad_end is_digit is_alpha is_space byte_len url_encode url_decode`.
`plum stdlib str` lists them with signatures.

## Operators

From lowest to highest precedence: `??` · `or` · `and` · `not` ·
`== != < <= > >= is in` · `.. ..=` · `|` · `^` · `&` · `<< >>` · `+ -` ·
`* / // %` · unary `- ~` · `**` · `as` · call, index, `.`, `?.`.

Comparisons don't chain (`a < b < c` is an error). `x in xs` tests
membership in a list, set, map (keys) or string. Compound assignment:
`+= -= *= /= //= %= **= &= |= ^= <<= >>=`.

## Collections

```plum
plant xs = [3, 1, 2]
xs.push(4)
drop(xs, len(xs), xs[0], xs[1..3], xs.sorted(), 2 in xs)
drop(xs.map((n) => n * 10), xs.filter((n) => n > 1), xs.reduce((a, n) => a + n, 0))
drop(xs.sorted_by((n) => -n), xs.any((n) => n > 3), xs.all((n) => n > 0), xs.find((n) => n > 2) ?? 0)
for i, x in xs.enumerate() {
    drop(i, x)
}

plant ages: map[str, int] = {:}
ages["Ada"] = 36
ages["Alan"] = 41
drop(ages, ages.get("Bob") ?? 0, "Ada" in ages, ages.keys())
for name, age in ages {
    drop("{name} is {age}")
}

plant seen = set([1, 2, 2])
seen.add(3)
drop(seen, len(seen))

plant a = [1, 2]
plant b = a                        # a copy: lists are values
b.push(3)
drop(a, b)
```

```output
[3, 1, 2, 4] 4 3 [1, 2] [1, 2, 3, 4] good
[30, 10, 20, 40] [3, 2, 4] 10
[4, 3, 2, 1] good good 3
0 3
1 1
2 2
3 4
{"Ada": 36, "Alan": 41} 0 good ["Ada", "Alan"]
Ada is 36
Alan is 41
set([1, 2, 3]) 3
[1, 2] [1, 2, 3]
```

| Type | Methods |
|---|---|
| `list[T]` | `push pop insert remove_at clear reverse sort sorted sorted_by contains index_of join map filter reduce any all find enumerate` |
| `map[K, V]` | `get remove keys values clear` (`m[k]` throws `KeyError` if missing; `m.get(k)` gives `V?`) |
| `set[T]` | `add remove contains clear` |

## Control flow

```plum
plant score = 85
if score >= 90 {
    drop("A")
} else if score >= 80 {
    drop("B")
} else {
    drop("C")
}

plant label = if score == 1 { "point" } else { "points" }

for i in 0..3 {                    # 0 1 2; 0..=3 includes 3
    if i == 1 {
        continue
    }
    drop(i)
}

plant n = 10
while n > 0 {
    n -= 4
}

plant command = "stop"
match command {
    "start" => drop("starting")
    "stop", "halt" => drop("stopping")
    else => drop("unknown")
}

plant grade = match score {
    90..=100 => "excellent"
    50..90 => "pass"
    else => "fail"
}
drop(label, n, grade)
```

```output
B
0
2
stopping
points -2 pass
```

## Functions

```plum
branch add(a: int, b: int) -> int {
    return a + b
}

branch square(x: int) -> int => x * x

branch connect(host: str, port: int = 80, secure: bool = bad) -> str {
    return "{host}:{port} secure={secure}"
}

branch greet(name: str) {          # no result type: returns none
    drop("Hello, {name}")
}

plant double = (x: int) => x * 2
plant apply = (f: (int) -> int, x: int) => f(x)

drop(add(2, 3), square(4), apply(double, 21))
drop(connect("example.com", secure: good))
greet("Ada")
```

```output
5 16 42
example.com:80 secure=good
Hello, Ada
```

Parameters are read-only. A function cannot change a caller's list; it
returns a new one, or takes a tree object, which is shared.

## Trees (classes), traits and kinds

```plum
trait Shape {
    branch area() -> float
    branch name() -> str => "shape"          # a default method
}

tree Circle : Shape {
    radius: float
    branch area() -> float => 3.14159 * self.radius ** 2
    branch name() -> str => "circle"
}

tree Account {
    owner: str
    balance: float = 0.0                     # fields with defaults may be left out
    _audit: list[str] = []                   # _names are private

    branch deposit(amount: float) {
        if amount <= 0 {
            throw ValueError("deposit must be positive")
        }
        self.balance += amount               # fields are always reached through self
        self._audit.push("deposit {amount}")
    }

    branch to_str() -> str => "{self.owner}: {self.balance:.2}"   # how it prints
}

tree Savings : Account {                     # single inheritance
    rate: float = 0.02
    branch deposit(amount: float) {
        super.deposit(amount * (1.0 + self.rate))
    }
}

tree Temperature {
    celsius: float
    branch init(fahrenheit: float) {         # a custom constructor
        self.celsius = (fahrenheit - 32.0) * 5.0 / 9.0
    }
}

kind Direction { North, East, South, West }

kind Payment {
    Cash
    Card(last4: str)
}

branch describe(p: Payment) -> str => match p {
    Cash => "cash"
    Card(digits) => "card ending {digits}"
}

plant acct = Account("Ada")
acct.deposit(50)
plant shared = acct                          # the same object, not a copy
shared.deposit(25)
plant saver = Savings("Grace")
saver.deposit(100)
plant shapes: list[Shape] = [Circle(1.0)]

drop(acct, saver, shapes[0].name(), shapes[0] is Shape)
drop(Temperature(212.0).celsius, Direction.North, describe(Payment.Card("4242")))
```

```output
Ada: 75.00 Grace: 102.00 circle good
100.0 Direction.North card ending 4242
```

Methods the language calls: `to_str() -> str` (printing), `equals(other: T)
-> bool` (`==`), `compare(other: T) -> int` (`<`, `sorted`), and `plus`,
`minus`, `times`, `divided_by`, `negate` (operators). Without `equals`, `==`
on tree objects asks whether they are the same object.

A `match` on a kind must cover every case, or end with `else`.

## Optionals

```plum
branch find_user(id: int) -> str? => if id == 1 { "ada" } else { none }

plant who = find_user(2)
drop(who ?? "nobody")                # a default
drop(find_user(1)?.upper())          # a none-safe call gives a str?
if plant name = find_user(1) {       # unwrap: name is a str inside
    drop("found {name}")
}
```

```output
nobody
ADA
found ada
```

## Errors

```plum
tree NotFound : Error {
    path: str
}

branch load(path: str) -> str {
    throw NotFound("no such file", path)
}

try {
    load("config.txt")
} catch e: NotFound {
    drop("missing: {e.path}")
} catch e {
    drop("other: {e.message}")
} finally {
    drop("done")
}

try {
    plant xs = [1]
    drop(xs[5])
} catch e: IndexError {
    drop("caught {type_of(e)}")
}
```

```output
missing: config.txt
done
caught IndexError
```

Built-in error trees: `Error` (the base, with `message`), `ValueError`,
`ParseError`, `IndexError`, `KeyError`, `OverflowError`,
`ZeroDivisionError`, `IOError`, `NetError`, `AssertError`. An uncaught error
prints a stack trace and exits with code 1.

## Modules and packages

```text
seed math                       # standard library: math.sqrt(2.0)
seed net.http                   # use as http.server()
seed net.http as web            # an alias
seed math { sqrt, PI }          # names brought in directly
seed ./shapes                   # shapes.plum next to this file: shapes.area(c)
seed ./models/user as user      # models/user.plum
```

Every file is a module; its top-level code runs once, on first import.
Imports come first in a file. Names starting with `_` are private. A folder
with a `plum.toml` (`[package]` with `name`, `version`, `entry`) is a
package: `plum run folder` runs its entry file.

## Tasks and channels

```plum
seed time

branch slow_square(n: int) -> int {
    time.sleep(0.01)
    return n * n
}

plant job = spawn slow_square(7)     # runs concurrently; no async/await
plant ch = Channel[int]()
spawn (() => {
    for i in 0..3 {
        ch.send(i)
    }
    ch.close()
})()

for n in ch {
    drop("got {n}")
}
drop(job.wait())
```

```output
got 0
got 1
got 2
49
```

Tasks run on one thread and share nothing: values are copied into a task,
and they talk through channels. A call that waits (network, files, timers,
channels) pauses only its own task.

## Standard library

| Module | For |
|---|---|
| `math` | numbers, constants, random numbers |
| `str` | string methods (always available) |
| `io` | standard input, output, error |
| `os` | arguments, environment, running commands |
| `time` | clocks, sleeping, dates |
| `fs` | files and directories |
| `json` | parse and produce JSON (`Json` values) |
| `net` | TCP, UDP, TLS, DNS |
| `net.http` | an HTTP(S) server and client |
| `hash` | SHA-256 digests |
| `path` | file paths: join, parent, name, extension |
| `base64`, `hex` | bytes as Base64 or hexadecimal text |
| `utf8` | code points; checking and repairing UTF-8 |
| `csv` | read and write CSV |
| `uuid` | unique identifiers: v4 (random) and v7 (time-ordered) |
| `semver` | semantic versions: compare, sort, ranges, bump |
| `url` | parse, build and join URLs; percent-encoding, query strings |
| `toml` | read and write TOML 1.0 (`Json` values) |

Built-in functions, no `seed` needed: `drop`, `ask`, `len`, `int`, `float`,
`str`, `byte`, `type_of`, `assert`, `exit`.

```plum
seed net.http

plant app = http.server()
app.get("/hello/:name", (req) => http.text("Hello, {req.params["name"]}!"))
app.listen(8080)
```

`plum stdlib` lists every function with its signature; each module has a
page in [docs/stdlib](stdlib/README.md).

## The plum command

```text
plum run program.plum [args...]    run a program (or a folder with plum.toml)
plum check program.plum            report every error without running
plum fmt files-or-folders...       format in place (--check, --stdout)
plum fmt --stdin                   format standard input to standard output
plum stdlib [module]               list the standard library
plum lsp                           the language server for editors (lsp.md)
plum run --check-leaks file.plum   report objects kept alive by cycles
plum run --mem-stats file.plum     report memory used
plum version
```

Exit codes: 0 success, 1 the program failed, 2 wrong usage, 3 unreadable
source. Errors give `file:line:column`, an error code (`E0402`), what is
wrong and a hint; [errors.md](errors.md) explains each code.

## For coding agents

- Run `plum check file.plum` after every edit, and fix the errors it reports
  before running. It reports all of them at once.
- When a method name is unsure, run `plum stdlib` or `plum stdlib MODULE`
  rather than guessing names from other languages.
- Format with `plum fmt`.
