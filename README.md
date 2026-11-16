<h1 align="center">Plumlang</h1>

<p align="center">
  A small, fast, memory-safe programming language. Easy to write, one binary, runs anywhere.<br>
  An <a href="https://elixpo.com">Elixpo</a> production.
</p>

<p align="center">
  <a href="https://plum.elixpo.com">Website</a> ·
  <a href="https://plum.elixpo.com/docs/">Documentation</a> ·
  <a href="docs/spec.md">Specification</a> ·
  <a href="https://github.com/plumlang/plumlang/issues/36">Roadmap</a> ·
  <a href="https://github.com/plumlang/plumlang/discussions">Discussions</a>
</p>

<p align="center">
  <a href="https://github.com/plumlang/plumlang/actions/workflows/build.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/plumlang/plumlang/build.yml?branch=main&style=flat-square&label=build"></a>
  <a href="https://github.com/plumlang/plumlang/releases"><img alt="Release" src="https://img.shields.io/github/v/release/plumlang/plumlang?include_prereleases&style=flat-square&label=release"></a>
  <a href="https://github.com/plumlang/plumlang/issues/36"><img alt="Status: alpha" src="https://img.shields.io/badge/status-alpha-orange?style=flat-square"></a>
  <a href="LICENSE"><img alt="Licence: Plumlang Commercial 1.0" src="https://img.shields.io/badge/licence-Plumlang%20Commercial%201.0-6b3fa0?style=flat-square"></a>
</p>

---

## About

Plumlang is a statically typed programming language with a syntax that reads
like plain English, written from scratch in C11 with no dependencies. Programs
are checked for type errors before they run, compiled to typed bytecode, and
run on a small virtual machine. Memory is managed by reference counting with
copy-on-write values, so it is freed the moment it is no longer used: no
garbage collector, no pauses, no manual `free`.

```plum
seed net.http

root GREETING = "Hello"

tree Greeter {
    name: str
    branch greet() -> str => "{GREETING}, {self.name}!"
}

plant app = http.server()
app.get("/hello/:name", (req) => http.text(Greeter(req.params["name"]).greet()))
app.listen(8080)
```

## Features

| | |
|---|---|
| **Easy to read and write** | A small set of keywords, and error messages with the file, line, column, what is wrong and how to fix it. |
| **Statically typed** | Types are inferred; optionals (`T?`, `??`, `?.`) take the place of null crashes. |
| **Memory-safe** | Bounds-checked collections and a runtime no program can corrupt; tested under AddressSanitizer and UndefinedBehaviorSanitizer. |
| **Object-oriented** | Classes (`tree`), single inheritance, traits, operator methods, `to_str` and `equals`. |
| **Expressive** | Closures, `map` / `filter` / `reduce`, kinds with `match`, `try` / `catch` / `finally`, modules. |
| **Concurrent** | Tasks (`spawn`) and channels on one thread, with an event loop over epoll, kqueue or WSAPoll. |
| **Batteries included** | `math`, `str`, `io`, `os`, `time`, `fs`, `json`, `net` (TCP, UDP, DNS, TLS), and an HTTP server and client. |
| **One binary** | A single `plum` for Linux, macOS and Windows, built with GCC, Clang or MSVC. |

## Install

Plumlang is in **alpha**; see [Releases](#releases). Build it from source in a few seconds with a C11 compiler and `make`:

```bash
git clone https://github.com/plumlang/plumlang
cd plumlang
make                         # builds ./plum
./plum run examples/bank.plum
```

On Windows with MSVC, use CMake: `cmake -B build && cmake --build build --config Release`.

## Usage

```bash
plum                                # the interactive prompt (REPL), see docs/repl.md
plum run program.plum [args...]     # run a program (or a directory with a plum.toml)
plum check program.plum             # report every error without running
plum fmt .                          # format every .plum file here (--check: only list them)
plum stdlib [module]                # list the standard library
plum lsp                            # the language server for editors, see docs/lsp.md
plum run --check-leaks program.plum # report objects left alive in reference cycles
plum run --mem-stats program.plum   # report how much memory the program used
```

## Documentation

| | |
|---|---|
| [Specification](docs/spec.md) | The language, section by section. The contract every change follows. |
| [Standard library](docs/stdlib/README.md) | Every module and function, generated from the code. |
| [Error codes](docs/errors.md) | Every compile error, with examples and fixes. |
| [Examples](examples/README.md) | Small complete programs, including a REST API and a multi-file package. |
| [Bytecode](docs/bytecode.md) | The instruction set of the virtual machine. |
| [REPL](docs/repl.md) | The interactive prompt: entries, commands and keys. |
| [Packages](docs/orchard.md) | Orchard: `plum add`, `plum.toml`, `plum.lock`, publishing, and the registry's formats. |
| [Language server](docs/lsp.md) | `plum lsp`: what editors get, and how to set it up in each. |

The same documentation, with a guided tour, is published at [plum.elixpo.com](https://plum.elixpo.com/docs/).

## Performance

Plumlang runs 1.2 to 2.6 times faster than Python 3.14 on loops, floating-point
arithmetic and text, and about even on deep recursion and method calls
([benchmarks](bench/README.md)). Its HTTP server answers about 120,000
requests a second on one thread, and 10,000 idle connections open in 0.16 s.

## Releases

| Version | Status |
|---|---|
| 1.0 | Planned. Specification frozen for 1.x compatibility, signed builds for every platform ([#35](https://github.com/plumlang/plumlang/issues/35)). |
| Pre-releases | See [Releases](https://github.com/plumlang/plumlang/releases). One-line installers, Homebrew and winget arrive with [#33](https://github.com/plumlang/plumlang/issues/33). |

Changes are recorded in [CHANGELOG.md](CHANGELOG.md).

## Roadmap

| Stage | Scope | Status |
|---|---|---|
| 1. Foundation | Specification, C scaffold, CI, licence, test harness | Done |
| 2. Front end | Lexer, parser, diagnostics | Done |
| 3. Semantic analysis | Scopes, type inference and checking | Done |
| 4. Runtime | Bytecode compiler, virtual machine, reference counting | Done |
| 5. Language and library | Collections, errors, OOP, modules, standard library, networking, HTTP, TLS | Done |
| 6. Tooling and 1.0 | Formatter, REPL, editor support, playground, releases, documentation site | In progress |

Every item is an issue; the full list is in [#36](https://github.com/plumlang/plumlang/issues/36).

## Repositories

| Repository | Contents |
|---|---|
| [plumlang](https://github.com/plumlang/plumlang) | The language (this repository). |
| [plum.elixpo](https://github.com/plumlang/plum.elixpo) | The website at plum.elixpo.com: documentation and playground. |
| [branding](https://github.com/plumlang/branding) | Logos, icons, and the VS Code extension. |

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) and the rules in
[PREAMBLE.md](PREAMBLE.md); coding agents start at [AGENTS.md](AGENTS.md).
Issues labelled [good first issue](https://github.com/plumlang/plumlang/issues?q=is%3Aopen+label%3A%22good+first+issue%22)
are a good place to begin, and [writing a standard module](docs/stdlib/writing-a-module.md)
walks through adding to the library. Questions go to
[Discussions](https://github.com/plumlang/plumlang/discussions) or
**hello@elixpo.com**; see [SUPPORT.md](SUPPORT.md).

## Licence

Plumlang is source-available under the
[Plumlang Commercial License 1.0](LICENSE). It is free for personal,
educational, research and other non-commercial use, and free to evaluate for
60 days. Commercial use, redistribution and hosted services need a commercial
licence: write to **hello@elixpo.com**. Programs you write in Plumlang are
yours. Third-party code it includes is listed in [LICENSES/](LICENSES/). See
also [PREAMBLE.md](PREAMBLE.md) and [TRADEMARKS.md](TRADEMARKS.md).

---

<p align="center"><sub>© 2026 Elixpo. Plumlang and the Plumlang logo are trademarks of Elixpo.</sub></p>
