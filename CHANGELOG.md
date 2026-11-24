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

- **Language** (#161): block comments `#[ ... ]#`, which span lines and nest; one never closed is E0140 at its `#[`. Doc comments: `##` lines directly before a `branch`, `tree`, `trait`, `kind`, `root`, field, method or kind case document it, and a file's first `##` lines document the module; the text is Markdown. `#[` at the start of a comment now opens a block comment, so an old `#[...` line comment needs a space (`# [...`). Spec section 2.2.
- **Tools** (#161): `plum fmt` keeps block comments as written and writes `##text` as `## text`; `plum lsp` shows doc comments on hover, and a module's doc on its `seed`; semantic tokens mark `##` comments with the `documentation` modifier, and the "add `seed`" quick fix goes after a leading block comment. `parser/doc.h` finds them for other tools (`plum doc`, #131).
- **Stdlib**: a `uuid` module (#154): `v4()` from the system's secure random source, `v7()` time-ordered (each one a program makes sorts after the one before, even within a millisecond), `parse` (either case, no hyphens, `{braces}` or `urn:uuid:`), `is_valid`, `version`, `variant`, `compare`, `timestamp` (of a v7) and the constants `NIL` and `MAX`. `docs/stdlib/uuid.md`, golden tests.
- **Stdlib**: a `semver` module (#157): `parse`, `is_valid`, `compare`, `sort`, `satisfies`, `max_satisfying`, `is_valid_range` and `bump`, for semantic versions with prereleases and build metadata (semver.org 2.0.0 precedence) and the version ranges of `plum.toml`. Orchard and the module share one implementation (`src/util/semver.c`, moved out of `src/orchard/manifest.c`); `plum.toml` and `plum.lock` read versions and ranges exactly as before. `docs/stdlib/semver.md`, golden tests.
- **Licence**: Plumlang is now Apache-2.0, with the Plumlang Commercial License as an option
  (`Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0`). v2.1.4 and earlier stay as released.
- **Stdlib**: five new modules, written in C. `path` (#91): `join`, `parent`, `name`, `stem`, `extension`, `is_absolute` and `path.SEPARATOR`, taking `\` as a separator too on Windows. `base64` (#72): `encode` / `decode` (RFC 4648, with padding) and `encode_url` / `decode_url` (URL-safe alphabet, no padding); decoding is strict and rejects whitespace. `hex` (#73): `encode` (optionally `upper`) and `decode`, either case. `utf8` (#74): `code`, `char`, `codes`, `from_codes`, strict `is_valid` (no overlong forms, surrogates or code points past U+10FFFF) and `fix`, which replaces each invalid sequence with U+FFFD. `csv` (#75): `parse`, `parse_records` and `stringify` (RFC 4180), with a choice of separator; an unclosed quote is a ParseError with its line. Each has a page in `docs/stdlib/` and golden tests (#25).
- **Stdlib**: a module constant can be a `str` (`PLUM_NATIVE_STR`), as `path.SEPARATOR` is.
- **Tooling** (#117): one source of truth for syntax highlighting. `tests/highlight/sample.plum` uses every keyword, literal form and string corner, and `tests/highlight/keywords.txt` lists the keywords (`tools/keywords.sh`, from the lexer's table); `make test` fails if they fall out of step with the lexer. The editor grammars test themselves against both in CI.
- **CLI**: `plum test [paths...] [--filter TEXT] [--json]` (#112) runs the `test_` functions of every `*_test.plum` file, each in a fresh program; a failure shows the error with its file and line and what the test printed, and exits 1. The new `test` module's `test.throws(f)` returns the error `f` throws, for checking expected errors. Spec section 14.3, `docs/test.md`.
- **Releases**: manifests for the AUR (`plum-bin`, `packaging/aur`) and Scoop (`packaging/scoop`, with `checkver` and `autoupdate`), filled by `tools/release/packaging.sh` with each release's version, checksums and licence; aqua-registry files for aqua and mise (`packaging/aqua`) and a from-source nixpkgs package (`packaging/nix`). `packaging/README.md` says how each is published. The release summary shows the checksums and the AUR files.
- **Playground**: the WebAssembly build is published to npm as `@plumlang/plumlang` from each release (`packaging/npm`, with TypeScript types; `tools/release/npm.sh`, `.github/workflows/npm.yml`). The release workflow publishes it when `NPM_TOKEN` is set.
- **Tooling**: `plum fmt --stdin` reads source from standard input and writes the formatted source to standard output, for editors that pipe a buffer through a formatter (Helix's `formatter`); syntax errors go to standard error with exit 1 and nothing on standard output (#113).
- **Benchmarks**: every benchmark in `bench/` also has Lua and Node.js versions, and there are four new ones (`jsondoc`, `textbuild`, `words`, `cart`). `bench/run.py` runs whichever interpreters are installed, checks their output matches before timing, and writes JSON; `bench/compare.py` compares two results. A CI job (`.github/workflows/bench.yml`) runs them on every pull request and fails one that makes a benchmark more than 15% slower than `main` (#116).
- **Tools**: `plum lsp` finds references and renames (#114): every use of a variable, parameter, function, tree, trait, kind, case, field, method or module alias in the package, imports and keyword arguments included, as one edit across files open or not; built-ins, the standard library and keywords are refused with the reason (docs/lsp.md).
- **Tools**: `plum lsp` signature help (#114): the signature of the call being typed, with the argument under the cursor (by position or by keyword) highlighted, for functions, methods, constructors, kind cases, built-ins and the standard library.
- **Tools**: `plum lsp` semantic tokens (#114): the whole file coloured by meaning (modules, types, trees, traits, kinds and cases, parameters, variables, fields, functions, methods; declarations, constants and what is built in), with keywords, numbers, strings and comments, in the protocol's standard legend.
- **Tools**: `plum lsp` quick fixes (#114): "Add `seed math`" (or `seed ./name` for a file beside it) for an unknown name that is a module, and "Prefix 'x' with '_'" for a name never used (W0502).
- **Performance**: release binaries are built with link-time and profile-guided optimisation (`make release-pgo`, trained on `bench/` and `examples/` by `tools/pgo-train.sh`): method dispatch (`cart`, `dispatch`) 17% faster than `-O2`, collatz, fib, jsondoc and words 11–12%, nothing slower. Linux x86_64 and arm64 get both; macOS arm64 PGO alone (Apple Clang's LTO builds fail the network tests), riscv64 LTO alone, and Windows MSVC's LTCG (`cmake -DPLUM_LTO=ON`). `make release-lto` is LTO alone. Measurements in `bench/README.md` (#115).

## v2.1.4 — 2026-11-16

The first public release of Plumlang: the language, its standard library and
its tools, as one dependency-free `plum` binary for Linux (x86_64, arm64,
riscv64), macOS (universal) and Windows (x64, arm64), plus a WebAssembly
build for the browser. Highlights: a statically typed language with trees,
traits, kinds and `match`; tasks and channels on an event loop; `json`,
`fs`, `net` (TCP, UDP, TLS) and an HTTP server and client; `plum fmt`, a
REPL, `plum lsp` for editors, and Orchard packages (`plum add`). Every
change below is tested on every platform, under the sanitizers and by
fuzzing.

- **Playground** (#32): `make wasm` builds plum for the browser with Emscripten (`build/wasm/plum.js` and `plum.wasm`, about 190 KB gzipped), with a JavaScript API to run, check and format programs (`docs/playground.md`). Runs are sandboxed, with step, memory and sleep limits, so an endless loop stops with a message; output streams as it is written; tasks and channels work. Releases carry it as `plum-X.Y.Z-wasm.tar.gz`, and `tests/wasm.sh` checks it prints what native `plum run` prints. `os.platform()` is `"wasm"` there.
- **Hardening** (#86): libFuzzer targets for the lexer, parser, checker, compiler and VM, json, interpolation formats and `net.http` (`make fuzz`), run on every pull request and nightly. Fixed what they and the limit tests found: a crash in `plum ast` on a field with no type; deep nesting crashing the compiler (now E0218 past 1000 levels); trees extending each other hanging `plum run` (E0427); hierarchies past 32 levels silently losing methods (E0428); trees past 64 fields unbuildable (now up to 255, E0429; parameters E0219); traits sharing ancestors taking exponential time; `json.parse` truncating long numbers and accepting `1e999`; `net.http` leaking ParseError and OverflowError on malformed messages. A `str` holds at most 1 GiB and a list 2^27 items (ValueError past that), and out of memory exits with an error instead of aborting.
- **VM**: a module variable used before its `plant` line has run is a runtime error, not a crash (#94).
- **Docs**: `docs/reference.md`, the whole language on one page for newcomers and coding agents; `make test` checks every code block on it and the output it shows. The spec now says `sorted_by` (not `sort_by`) and that `==` tests identity for trees without `equals` (not `is`), as implemented.
- **Packages**: Orchard, the package registry (`docs/orchard.md`, spec 9.3): `plum add`, `remove`, `install`, `package`, `publish` and `search`; `plum.toml` read by a strict TOML subset with errors at their line and column (E09xx); `plum.lock` with exact versions and sha256 checksums; semver ranges `^ ~ = >= *` resolved to the highest version that isn't yanked; a shared cache in `~/.plum/orchard` (`PLUM_HOME`); reproducible `.plumpkg` bundles, checked before unpacking; `seed name` and `seed name.sub` of declared packages through the lock. The index URL is `PLUM_ORCHARD`.
- **Stdlib**: the `hash` module, with `hash.sha256(text)` as lowercase hex (Mbed TLS's SHA-256).
- **Tools**: `plum lsp`, a language server for editors over stdio (docs/lsp.md): the diagnostics of `plum check` as you type, hover with types and signatures, go to definition (across `seed ./file` imports), completion of names in scope and of members after `.`, the document outline, and formatting with `plum fmt`'s layout. Imports between open files use the editor's unsaved text. Set-up for VS Code, Neovim, Helix, Zed and Emacs.
- **CLI**: `plum fmt` formats source files in one canonical layout (spec 14.1), in place or `--stdout`, files or whole directories; `--check` lists the files that would change, for CI; `plum fmt -` formats standard input. Comments are kept, literals keep their spelling, and a file with syntax errors is left alone. `net.http` is formatted with it.
- **Releases**: a tagged release builds static Linux binaries (x86_64, arm64, riscv64), a universal macOS binary and Windows x64 and ARM64 binaries, checks each by installing it, and publishes them with `SHA256SUMS` and build attestations; `install.sh` and `install.ps1`; a Homebrew formula and winget manifests; signing for macOS and Windows once its secrets are set. See `docs/releasing.md`.
- **Spec**: `docs/spec.md`, the language specification v0.1 draft.
- **CLI**: the `plum` command with `help`, `version` and `run`; `run` reads the source file until the compiler lands.
- **Build**: Makefile (release, debug, sanitize) and CMake (MSVC, IDEs); warnings are errors.
- **Core utilities**: checked allocation, an arena allocator, growable arrays, a string-keyed hash map and string interning, with unit tests run by `make test`.
- **Lexer**: tokens with line and column, string interpolation, triple-quoted strings, errors that explain foreign spellings (`&&`, `!`, `++`, `;`); `plum tokens` prints them.
- **Parser**: expressions and types with the spec's precedence, lambdas, interpolation, one error per mistake; `plum ast` prints the tree.
- **Parser**: statements and declarations (functions, trees, traits, kinds, seed, try, match), recovering after errors without losing the next line.
- **Errors**: each error shows the source line with the mistake underlined, in colour on a terminal (`NO_COLOR` / `FORCE_COLOR`), and a count at the end.
- **Methods**: methods with `self`, `init`, overriding and `super`, traits with default methods, `x is Trait`, `to_str` for printing and `equals` for `==`; private `_names`; lists and maps in fields change in place. Checks for trait methods, override types, `init` and private names (E0420–E0424). Examples in `examples/` and a `dispatch` benchmark.
- **Kinds and match**: kind values with and without contents, compared by value and printed as written; `match` on cases (binding their values), literals, ranges and root constants, as a statement or a value.
- **Weak references**: `weak` fields that don't keep their object alive (E0425 for other types); `plum run --check-leaks` reports objects left in reference cycles.
- **Standard library**: `seed math` (also `as` and `{ names }`) for standard modules; `math` (functions, constants, seedable random numbers), the methods of `str`, `io`, `os` (arguments, environment, running commands), `time` (clocks, sleep, dates) and `fs` (files and directories); `plum stdlib` lists them; docs in `docs/stdlib/`. Modules are C functions described by Plumlang signatures.
- **Modules**: `seed ./file` (also `as` and `{ names }`, and `module.Type` in types); each file loads and runs once, in import order; import cycles (E0311) and missing files are reported at the `seed`; `_names` stay private; `plum run <dir>` runs a package's `plum.toml` entry; errors in imported files name that file. `examples/garden` is a multi-file program.
- **JSON**: `json.parse` and `json.stringify` (compact or indented), and the `Json` type for any JSON value, with methods (`get`, `at`, `str`, `int`, `list`, ...) that give `none` instead of failing; plain data, including tree objects, can be written as JSON.
- **Tasks**: `spawn`, `task.wait()` and `done()`, `Channel[T](capacity)` with `send`, `receive`, `close` and `for x in channel`; `time.sleep` lets other tasks run. All tasks share one thread and take turns where they wait; thousands cost a few megabytes. Deadlocks and errors in tasks nothing waited for are reported; objects can't be given to tasks (E0426).
- **Networking**: the `net` module: TCP (`listen`, `accept`, `connect`, `read`, `read_line`, `write`), UDP (`udp`, `send_to`, `receive`) and `resolve`, on POSIX sockets and Winsock; waiting on a socket lets other tasks run, every wait takes a `timeout`. The scheduler waits on sockets with epoll, kqueue or WSAPoll and keeps timers in a heap: 10,000 idle connections open in 0.16 s. `examples/echo.plum`.
- **HTTP**: `seed net.http`: a server (routes with `:params`, query strings, JSON, middleware, static files, keep-alive, a task per connection) and a client (`get`, `post`, `post_json`, `request`; chunked bodies). Written in Plumlang and built into the binary, like any standard module written that way. About 120,000 requests a second on one thread. `examples/http/`.
- **TLS**: `net.tls_connect` (checks the server's certificate against the system's trusted CAs, or `ca_file`, and the host name; `verify: bad` turns that off) and `net.listen_tls` (certificate and key from PEM files); TLS connections read and write like any other, and their handshakes let other tasks run. `https://` URLs in the `net.http` client, and `app.start_tls` / `app.listen_tls` for HTTPS servers. Mbed TLS 4.1.1 is vendored in `vendor/mbedtls` and built into `plum`.
- **Operators on objects**: `+ - * /` and unary `-` call a tree's `plus`, `minus`, `times`, `divided_by` and `negate`; `< <= > >=` call `compare`, which also lets `sort()` and `sorted()` order lists of objects. Lists of objects with `equals` use it for `contains`, `index_of`, `in` and `==`. The methods' shapes are checked (E0424).
- **Memory statistics**: `plum run --mem-stats` prints, when the program ends, the peak bytes in use, the bytes allocated, what is still in use, the number of allocations, resizes and frees, and the values made by kind.
- **Deep values**: `==` on values nested hundreds of thousands deep no longer overflows the C stack; printing writes `...` past a depth of 256. Stress tests for deep and large structures.
- **REPL**: `plum` on a terminal (or `plum repl`) runs entries as they are typed, keeping what earlier entries declared (variables, functions, trees, kinds, `seed` imports); an entry that is one expression prints `= value : type`; entries continue over lines while a bracket is open or a line ends with an operator; errors leave the session going; tasks carry on between entries. `:help`, `:type`, `:load`, `:reset`, `:quit`. A built-in line editor (cursor keys, Home/End, Ctrl-A/E/U/K/W/L/C/D, history in `~/.plum_history`) on POSIX terminals and the Windows console. `docs/repl.md`.
- **Tests**: golden tests can leave out lines that vary between machines with `# ignore: REGEX`.
- **str**: `byte_len`, `url_encode`, `url_decode`.
- **Parser**: `else` may start the line after an `if` expression's block; a line starting with a stray `else` no longer hangs the parser.
- **Parser**: a keyword used as a field name is E0217 instead of an endless loop.
- **Memory**: freeing a long chain of objects no longer recurses; callbacks nest at most 100 deep.
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
