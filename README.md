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
  <a href="LICENSE"><img alt="Licence: Apache-2.0" src="https://img.shields.io/badge/licence-Apache--2.0-6b3fa0?style=flat-square"></a>
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
| **Batteries included** | `math`, `str`, `io`, `os`, `time`, `fs`, `path`, `json`, `csv`, `base64`, `hex`, `utf8`, `hash`, `net` (TCP, UDP, DNS, TLS), and an HTTP server and client. |
| **One binary** | A single `plum` for Linux, macOS and Windows, built with GCC, Clang or MSVC. |

## Install

```bash
curl -fsSL https://plum.elixpo.com/install.sh | sh     # Linux and macOS
irm https://plum.elixpo.com/install.ps1 | iex          # Windows (PowerShell)
brew install plumlang/tap/plum                         # Homebrew
```

The installers check every download against the release's `SHA256SUMS`; see
[the install guide](https://plum.elixpo.com/docs/install/) for versions and
install locations. To try Plumlang without installing anything, use the
[playground](https://plum.elixpo.com/playground/). To build from source you
need a C11 compiler and `make`:

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
plum fmt --stdin < program.plum     # format standard input to standard output, for editors
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
| [Playground](docs/playground.md) | Plumlang in the browser: `make wasm`, the npm package [`@plumlang/plumlang`](https://www.npmjs.com/package/@plumlang/plumlang), and the JavaScript API for embedding it. |

The same documentation, with a guided tour, is published at [plum.elixpo.com](https://plum.elixpo.com/docs/).

## Performance

Plumlang runs 1.2 to 2.6 times faster than Python 3.14 on loops, floating-point
arithmetic and text, and about even on deep recursion and method calls
([benchmarks](bench/README.md)). Its HTTP server answers about 120,000
requests a second on one thread, and 10,000 idle connections open in 0.16 s.

## Releases

The current release is on [Releases](https://github.com/plumlang/plumlang/releases):
eight builds for Linux, macOS and Windows, with `SHA256SUMS` and build
attestations. The next one, 2.2.0, ships the `path`, `base64`, `hex`, `utf8`
and `csv` modules and the Apache-2.0 licence
([milestone](https://github.com/plumlang/plumlang/milestone/7)). Changes are
recorded in [CHANGELOG.md](CHANGELOG.md).

## Roadmap

The language, its runtime and its standard library are complete. What comes
next is staged in milestones, each a set of issues with a "done when":

| Milestone | Scope |
|---|---|
| [2.2: Ship and polish](https://github.com/plumlang/plumlang/milestone/7) | The 2.2.0 release, `plum test`, a deeper language server, benchmarks in CI |
| [2.3: Fast VM](https://github.com/plumlang/plumlang/milestone/8) | Typed opcodes, fixed field offsets, cheaper calls: at least 5x Python |
| [2.4: Batteries](https://github.com/plumlang/plumlang/milestone/9) | `regex`, `datetime`, `process`, a C FFI, embedding, Orchard 1.0, `plum doc` |
| [3.0: Native and ready](https://github.com/plumlang/plumlang/milestone/10) | A native compiler, multicore tasks, a debugger and profiler, a frozen spec |

[#137](https://github.com/plumlang/plumlang/issues/137) is the checklist for
calling Plumlang ready for production; the whole roadmap is
[#36](https://github.com/plumlang/plumlang/issues/36).

## Ecosystem

| | |
|---|---|
| **VS Code** | [Plumlang extension](https://marketplace.visualstudio.com/items?itemName=plum-lang.plum-lang) on the Marketplace: highlighting, errors as you type, hover, go to definition, completion, formatting. Also on [Open VSX](https://open-vsx.org/extension/plum-lang/plum-lang) for VSCodium and Cursor. |
| **Zed** | [zed-plum](https://github.com/plumlang/zed-plum), in review for Zed's extension gallery; install it now as a dev extension. |
| **Helix** | Built-in support in review ([helix#16493](https://github.com/helix-editor/helix/pull/16493)). |
| **Other editors** | Any editor with LSP support can run `plum lsp`; see [the language server guide](https://plum.elixpo.com/docs/tools/lsp/). |
| **Tree-sitter** | [tree-sitter-plum](https://github.com/plumlang/tree-sitter-plum): the grammar for Neovim, Helix, Zed and others; npm `@plumlang/tree-sitter-plum`. |
| **Highlighting on the web** | [highlightjs-plum](https://github.com/plumlang/highlightjs-plum) for highlight.js (npm `@plumlang/highlightjs-plum`) and [pygments-plum](https://github.com/plumlang/pygments-plum) for Pygments, Sphinx and MkDocs (PyPI `pygments-plum`). |
| **In JavaScript** | `@plumlang/plumlang` on npm: run, check and format Plumlang in Node or the browser (WebAssembly), from 2.2.0. |
| **CI** | [setup-plum](https://github.com/plumlang/setup-plum): `uses: plumlang/setup-plum@v1` installs a verified `plum` in GitHub Actions. |
| **Packages** | [Orchard](https://plum.elixpo.com/packages/), the package index: `plum add`, `plum publish`. |

Published packages (the full list is at [plum.elixpo.com/docs/ecosystem](https://plum.elixpo.com/docs/ecosystem/)):

| Package | Registry | What |
|---|---|---|
| [`@plumlang/tree-sitter-plum`](https://www.npmjs.com/package/@plumlang/tree-sitter-plum) | npm | The Tree-sitter grammar |
| [`@plumlang/highlightjs-plum`](https://www.npmjs.com/package/@plumlang/highlightjs-plum) | npm | highlight.js support, also on [jsDelivr](https://cdn.jsdelivr.net/npm/@plumlang/highlightjs-plum/dist/plum.min.js) |
| [`pygments-plum`](https://pypi.org/project/pygments-plum/) | PyPI | The Pygments lexer |
| [`@plumlang/plumlang`](https://www.npmjs.com/package/@plumlang/plumlang) | npm | `plum` as WebAssembly for Node and the browser, from 2.2.0 |
| [Plumlang](https://marketplace.visualstudio.com/items?itemName=plum-lang.plum-lang) | VS Code Marketplace, [Open VSX](https://open-vsx.org/extension/plum-lang/plum-lang) | The editor extension |
| [`plumlang/tap/plum`](https://github.com/plumlang/homebrew-tap) | Homebrew | `brew install plumlang/tap/plum` |
| [`plumlang/setup-plum@v1`](https://github.com/plumlang/setup-plum) | GitHub Actions | Installs `plum` in CI |

## Repositories

| Repository | Contents |
|---|---|
| [plumlang](https://github.com/plumlang/plumlang) | The language: compiler, virtual machine, standard library, language server, specification and tests (this repository). |
| [plum.elixpo](https://github.com/plumlang/plum.elixpo) | The website at plum.elixpo.com: documentation, packages and the playground. |
| [registry](https://github.com/plumlang/registry) | Orchard, the package index. |
| [branding](https://github.com/plumlang/branding) | Logos, icons, colours and the VS Code extension. |
| [tree-sitter-plum](https://github.com/plumlang/tree-sitter-plum) | The Tree-sitter grammar. |
| [zed-plum](https://github.com/plumlang/zed-plum) | The Zed extension. |
| [highlightjs-plum](https://github.com/plumlang/highlightjs-plum) | The highlight.js grammar. |
| [pygments-plum](https://github.com/plumlang/pygments-plum) | The Pygments lexer plugin. |
| [setup-plum](https://github.com/plumlang/setup-plum) | The GitHub Action that installs `plum`. |
| [homebrew-tap](https://github.com/plumlang/homebrew-tap) | The Homebrew formula. |

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) and the rules in
[PREAMBLE.md](PREAMBLE.md); coding agents start at [AGENTS.md](AGENTS.md).
Issues labelled [good first issue](https://github.com/plumlang/plumlang/issues?q=is%3Aopen+label%3A%22good+first+issue%22)
are a good place to begin, and [writing a standard module](docs/stdlib/writing-a-module.md)
walks through adding to the library. Questions go to
[Discussions](https://github.com/plumlang/plumlang/discussions) or
**hello@elixpo.com**; see [SUPPORT.md](SUPPORT.md).

## Licence

Plumlang is licensed under the [Apache License 2.0](LICENSE), or, as an
alternative, the
[Plumlang Commercial License 1.0](LICENSES/preferred/LicenseRef-Plumlang-Commercial-1.0)
(`Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0`). Anyone may use, modify
and redistribute it under Apache-2.0, commercially or not. The commercial
licence is for those who want a written agreement with Elixpo, for example
for warranty, support, indemnity or trademark terms that Apache-2.0 does not
give: write to **hello@elixpo.com**. Programs you write in Plumlang are
yours. The names and logos are not licensed by either; see
[TRADEMARKS.md](TRADEMARKS.md). Third-party code it includes is listed in
[LICENSES/](LICENSES/). See also [PREAMBLE.md](PREAMBLE.md).

---

<p align="center"><sub>© 2026 Elixpo. Plumlang and the Plumlang logo are trademarks of Elixpo.</sub></p>
