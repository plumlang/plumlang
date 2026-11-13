# Changelog

Each release of Plumlang gets its own section here, newest first. The
release notes on GitHub are taken from the top section.

Format for a new entry:

```
## vX.Y.Z — YYYY-MM-DD
- Short bullets, present tense (e.g. "Report the column in type errors").
- Group bullets loosely by area: Language, Compiler, VM, Stdlib, CLI, Docs.
- Keep technical, no marketing language.
```

---

## Unreleased

- **Spec**: `docs/spec.md`, the language specification v0.1 draft.
- **CLI**: the `plum` command with `help`, `version` and `run`; `run` reads the source file until the compiler lands.
- **Build**: Makefile (release, debug, sanitize) and CMake (MSVC, IDEs); warnings are errors.
- **Core utilities**: checked allocation, an arena allocator, growable arrays, a string-keyed hash map and string interning, with unit tests run by `make test`.
- **Lexer**: tokens with line and column, string interpolation, triple-quoted strings, errors that explain foreign spellings (`&&`, `!`, `++`, `;`); `plum tokens` prints them.
- **Parser**: expressions and types with the spec's precedence, lambdas, interpolation, one error per mistake; `plum ast` prints the tree.
- **Parser**: statements and declarations (functions, trees, traits, kinds, seed, try, match), recovering after errors without losing the next line.
- **Errors**: each error shows the source line with the mistake underlined, in colour on a terminal (`NO_COLOR` / `FORCE_COLOR`), and a count at the end.
- **Error codes**: every error has a code (`error[E0201]`) explained in `docs/errors.md`.
- **Names**: `plum check` reports unknown names (with "did you mean"), duplicates, use before declaration and changes to constants; `plum run` checks first.
- **Types**: `plum check` type-checks programs: inference, int-to-float only, no truthiness, optionals that must be unwrapped, checked calls, trees and kinds.
- **Control flow**: missing returns, `break`/`continue` outside loops and non-exhaustive `match` are errors; unreachable code and unused variables are warnings.
- **Compiler**: checked programs compile to typed bytecode; `plum dis` prints it, documented in `docs/bytecode.md`.
- **VM**: `plum run` runs programs. Integers never wrap (overflow is an error); runtime errors name the line and the calls that led there; interpolation formats (`{x:.2}`, `{n:>5}`, `{n:x}`).
- **Benchmarks**: `bench/` compares Plumlang with Python (1.2x to 2.6x faster on loops, floats and text); typed number equality.
- **Lists**: `list[T]` values with copy-on-write: literals, indexing, slices, `in`, `+`, `for` and `enumerate()`, and methods (`push`, `pop`, `insert`, `remove_at`, `sort`, `sorted`, `join`, ...).
- **Maps and sets**: `map[K, V]` and `set[T]` values in insertion order, hashed with a randomly keyed SipHash; `m[k]`, `get`, `remove`, `keys`, `values`, `for k, v in m`, `add`, `in`.
- **Strings as sequences**: `s[i]`, `s[a..b]`, `for ch in s` and `sub in s`, all by character, not byte.
- **Nested collections**: `grid[i][j] = v`, `m["k"].push(x)`, with copy-on-write at every level; `x in a..b`. Ranges are written where they are used, not stored.
- **Closures**: lambdas and nested functions capture variables and share them; each loop iteration captures its own value.
- **Higher-order list methods**: `map`, `filter`, `reduce`, `any`, `all`, `find`, `sorted_by`.
- **Trees**: objects built by position, by name or from field defaults; fields read and set (objects are shared); `?.` on fields; `x is Tree` through inheritance; the built-in error trees with a `message`.
- **Errors**: `throw`, `try`/`catch`/`finally` and `try expr`; the language's own runtime errors (overflow, bad index, missing key, …) are catchable error trees; an uncaught error prints its tree's name, message and where it was thrown.
- **Tests**: golden-file tests (`tests/lang/`) with pending markers and `make golden-update`; the Python prototype's programs ported to the spec's syntax.
- **CI**: builds and tests on Linux (x86_64, arm64), macOS and Windows (MSVC, mingw-w64), under ASan and UBSan, with a clang-format check.
- **Licensing and governance**: the Plumlang Commercial License 1.0 with its
  preamble, the `LICENSES/` tree, trademarks, contributing guide, security
  policy, issue forms and CI.
