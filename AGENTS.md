# AGENTS.md

Guide for coding agents, and for people, working in this repository. Read it
before changing anything. The binding rules every contribution follows,
licensing included, are in [PREAMBLE.md](PREAMBLE.md).

## What Plumlang is

Plumlang is a small, fast, memory-safe programming language, written in C11,
an Elixpo production. Source code goes through a lexer, parser, type checker
and compiler to bytecode, which a virtual machine runs. Memory is managed by
reference counting with copy-on-write values. Everything ships as one `plum`
binary with no dependencies. The language specification is
[`docs/spec.md`](docs/spec.md); the roadmap is issue
[#36](https://github.com/plumlang/plumlang/issues/36).

## How we build it

1. **One binary, no dependencies.** C11, the C standard library and OS APIs
   only. Vendored third-party code needs maintainer approval and a permissive
   licence (PREAMBLE 1.3).
2. **It runs everywhere.** Linux, macOS and Windows; GCC, Clang and MSVC.
   Anything compiler-specific has a portable fallback.
3. **Easy to write comes first.** Syntax favours what a beginner reads
   correctly. Every error gives file, line, column, what is wrong and, when
   possible, a fix.
4. **Memory-safe by construction.** No Plumlang program can corrupt the
   runtime's memory. Tests run clean under ASan and UBSan.
5. **The spec is the contract.** A language change updates `docs/spec.md`,
   the tests and the implementation in the same pull request.
6. **Every change is tested** with golden-file tests under `tests/`.

## Language at a glance

Keywords keep the garden theme of the original prototype: `plant` (variable),
`root` (constant), `branch` (function), `drop` (print), `tree` (class),
`seed` (import), `good` / `bad` (true / false). The spec is authoritative;
don't invent syntax that isn't in it.

## Map

| Path | What |
|---|---|
| `src/lexer/` | Tokens with line and column. |
| `src/parser/` | Syntax tree (arena-allocated) and syntax errors. |
| `src/check/` | Name resolution, type inference and checking. |
| `src/compiler/` | Checked tree to bytecode. |
| `src/vm/` | Bytecode interpreter, values, reference counting. |
| `src/stdlib/` | Built-in modules: io, math, string, fs, time, json, net, http. |
| `src/cli/` | The `plum` command. |
| `src/util/` | Allocation wrappers, arrays, hash maps, string interning. |
| `include/plum/` | Public headers. |
| `tests/` | Golden-file tests: `name.plum` with `name.out` or `name.err`. |
| `docs/` | Spec, bytecode format, error codes, security, governance. |

Directories appear as their roadmap stage lands.

## C rules

- C11, warnings as errors. No VLAs. No extensions without a fallback.
- Prefixes: `plum_` for functions, `Plum` for types, `PLUM_` for macros.
- No mutable global state; state lives in `PlumVM` / `PlumCompiler`.
- Allocate only through `plum_alloc` and friends; bounds-check every write.
- New files start with
  `/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */`.
- Format with the repository's `.clang-format`.

## Checks

```bash
make                 # build ./plum
make test            # golden-file tests
make sanitize test   # under ASan + UBSan
```

## Workflow

- One folder per repository. Work on a branch named after the issue
  (`area/issue-N`), open a pull request, and squash-merge. `main` is
  protected.
- One pull request per issue, single author.
- Pull requests say what changed, what was tested, and on which platforms.
- Commits are signed off (`git commit -s`) and carry no AI attribution
  trailers.
- User-visible changes get a line under `## Unreleased` in `CHANGELOG.md`.

## Do not

- Add a dependency, or vendor code, without maintainer approval.
- Change the language without changing `docs/spec.md`.
- Change `LICENSE`, `PREAMBLE.md`, `TRADEMARKS.md` or anything under
  `LICENSES/` unless the maintainer asked.
- Add telemetry or any network access the running program didn't ask for.
- Commit secrets or personal data.
- Tag releases or publish packages unless the maintainer asked.
