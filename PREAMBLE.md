# Plumlang: preamble

<p>
  <a href="LICENSE"><img alt="Licence: Apache-2.0" src="https://img.shields.io/badge/licence-Apache--2.0-6b3fa0?style=flat-square"></a>
  <a href="TRADEMARKS.md"><img alt="Trademarks: Elixpo" src="https://img.shields.io/badge/trademarks-Elixpo-333333?style=flat-square"></a>
</p>

Plumlang, its compiler, virtual machine, standard library and tools, is
provided under:

    SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0

being under the terms of the Apache License, version 2.0, according with:

    LICENSE
    LICENSES/preferred/Apache-2.0

or, alternatively, under the terms of the Plumlang Commercial License,
version 1.0, according with:

    LICENSES/preferred/LicenseRef-Plumlang-Commercial-1.0

Anyone may use, modify and redistribute Plumlang under Apache-2.0,
commercially or not. The Plumlang Commercial License is an option for those
who hold a written commercial agreement with Elixpo (hello@elixpo.com), for
example for warranty, support, indemnity or trademark terms that Apache-2.0
does not give. Programs written in the Plumlang language belong to their
authors.

The Elixpo and Plumlang names and logos are not licensed by either of the
above (Apache-2.0 section 6 grants no trademark rights); see:

    TRADEMARKS.md
    LICENSES/NOTICE

All contributions to Plumlang are subject to this preamble, and to the rules
below.

---

## 1. Licensing rules

1.1. **One licence expression for the tree.** Unless a file states
otherwise, every file in this repository (source code, build files, scripts,
tests, documentation, examples and brand assets) is licensed under
`Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0`.

1.2. **New source files say so on their first line.** A new C source or
header file starts with:

    /* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */

Other files may carry the same line as a comment in their own syntax. No file
may name a different licence, except under rule 1.3.

1.3. **Third-party code keeps its licence.** Code copied or vendored from
elsewhere lives under `vendor/`, keeps its original licence and copyright
lines, is listed in `LICENSES/notices/notice-board`, and needs maintainer
approval before it is merged. Only permissive licences (MIT, BSD, Apache-2.0,
zlib, ISC) are accepted: nothing copyleft is linked into the `plum` binary.

1.4. **Inbound licence and grant.** A contribution is submitted under
Apache-2.0 (section 5 of [`LICENSE`](LICENSE)). In addition, by submitting a
contribution (including code, documentation, or issues) you grant Elixpo a
perpetual, worldwide, irrevocable, royalty-free license to use, modify,
sublicense, and relicense your contribution under any terms, including
commercial terms, such as the Plumlang Commercial License. You represent
that you have the right to grant this license. There is no separate CLA.

1.5. **Every commit is signed off.** The
[Developer Certificate of Origin](https://developercertificate.org) applies to
every commit; sign off with `git commit -s`.

1.6. **Licence texts live in `LICENSES/`.** Its layout mirrors the Linux
kernel's: `preferred/` holds the licence texts in use, `exceptions/` and
`notices/` the rest. A licence is added there before anything may use it.

## 2. Engineering rules

These decide most design questions before they come up. A change that breaks
one is not merged.

2.1. **One binary, no dependencies.** Plumlang is written in C11 against the
C standard library and the operating system's own APIs. `plum` is a single
executable; installing it never needs another runtime, package manager or
shared library.

2.2. **It runs everywhere.** Every change builds and passes the tests on
Linux, macOS and Windows, with GCC, Clang and MSVC. Compiler-specific code
(computed goto, builtins) always has a portable fallback. No assumptions about
word size or byte order beyond what C11 guarantees.

2.3. **Easy to write comes first.** When two designs are equal in power, the
one a beginner reads correctly wins. Every error message says where the
problem is (file, line, column), what is wrong, and, when it can, how to fix
it.

2.4. **Memory-safe by construction.** No Plumlang program can cause a
use-after-free, double free, buffer overflow or uninitialised read in the
runtime. Memory is reclaimed deterministically by reference counting; there
is no garbage collector pause and no manual freeing. The whole test suite
runs clean under AddressSanitizer and UndefinedBehaviorSanitizer.

2.5. **The specification is the contract.** [`docs/spec.md`](docs/spec.md)
says what the language means. A change to the language changes the spec, the
tests and the implementation in the same pull request.

2.6. **Every change is tested.** New behaviour comes with a test under
`tests/`. A bug fix comes with the test that would have caught it.

2.7. **Small and fast.** A new feature justifies its size in the binary and
its cost at run time. Performance regressions show up in `bench/` and are
explained in the pull request.

2.8. **No telemetry.** `plum` never contacts the network unless the program
it runs asks it to.

## 3. C coding rules

3.1. C11, compiled with warnings as errors (`-Wall -Wextra -Wpedantic` and
`/W4` on MSVC). No variable-length arrays, no compiler extensions without a
fallback.

3.2. Public symbols start with `plum_`, types with `Plum`, macros with
`PLUM_`. Headers that other parts include live in `include/plum/`.

3.3. No mutable global state: everything belongs to a `PlumVM` or a
`PlumCompiler`, so Plumlang can be embedded and run more than once in a
process.

3.4. Every allocation goes through the `plum_alloc` family, which checks for
failure. Every buffer write is bounds-checked.

3.5. Code is formatted with the repository's `.clang-format`.

## 4. Process

4.1. Work happens on a branch and lands through a pull request,
squash-merged; `main` is protected.

4.2. One pull request does one thing and closes one issue. Each pull request
has a single author.

4.3. A pull request says what changed, what was tested, and on which
platforms.

4.4. Before a pull request:

    make
    make test
    make sanitize test

4.5. Security issues are reported privately, as
[`docs/SECURITY.md`](docs/SECURITY.md) describes, never in a public issue.
How decisions are made is in [`docs/GOVERNANCE.md`](docs/GOVERNANCE.md); how
contributors behave, in [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).

## 5. What Plumlang is

Plumlang is a statically typed, garbage-collector-free programming language
with an easy syntax, object-oriented programming, and a standard library
that covers files, networking and HTTP backends. A program is compiled to
bytecode and run by a small virtual machine, all inside one `plum` binary.

| Stage | Where | What it does |
| --- | --- | --- |
| Lexer | `src/lexer/` | Turns source text into tokens with line and column |
| Parser | `src/parser/` | Builds the syntax tree and reports syntax errors |
| Checker | `src/check/` | Resolves names and checks types before anything runs |
| Compiler | `src/compiler/` | Turns the checked tree into bytecode |
| Virtual machine | `src/vm/` | Runs bytecode; owns memory and reference counting |
| Standard library | `src/stdlib/` | io, math, strings, files, time, json, net, http |
| CLI | `src/cli/` | The `plum` command: run, repl, fmt, version |

The roadmap is [#36](https://github.com/plumlang/plumlang/issues/36). Where
to go next: [`CONTRIBUTING.md`](CONTRIBUTING.md) to contribute,
[`docs/spec.md`](docs/spec.md) for the language, and
[`AGENTS.md`](AGENTS.md) for the same rules, written for coding agents.
