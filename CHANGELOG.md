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
- **Tests**: golden-file tests (`tests/lang/`) with pending markers and `make golden-update`; the Python prototype's programs ported to the spec's syntax.
- **CI**: builds and tests on Linux (x86_64, arm64), macOS and Windows (MSVC, mingw-w64), under ASan and UBSan, with a clang-format check.
- **Licensing and governance**: the Plumlang Commercial License 1.0 with its
  preamble, the `LICENSES/` tree, trademarks, contributing guide, security
  policy, issue forms and CI.
