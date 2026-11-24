# Benchmarks

Each `NAME.plum` here has the same program in Python (`python/NAME.py`),
Lua (`lua/NAME.lua`) and Node.js (`node/NAME.mjs`). `python3 bench/run.py`
runs every one whose interpreter is installed (a missing one is skipped
with a note), first checks that it prints exactly what the Plumlang version
prints, and only then times it: fastest of three runs, or `--runs N`.

| Benchmark | Exercises |
|---|---|
| `fib` | function calls: fib(30), about 2.7 million calls |
| `loop` | a 20-million-step range loop with int arithmetic |
| `collatz` | while loops, `//` and `%`, comparisons |
| `mandelbrot` | float arithmetic in nested loops |
| `strings` | interpolation with formats, `len` |
| `dispatch` | 3 million method calls through a trait, and a method changing its object |
| `jsondoc` | `json.stringify` and `json.parse` of a 20,000-record document, five times, and walking the parsed result |
| `textbuild` | building text: `+=` a line at a time (20,000 lines), and `join` of 600,000 pieces |
| `words` | a hash-map workload: counting the words of a 1,000,000-word text in a `map[str, int]` |
| `cart` | method dispatch over a collection: 4 million calls through a trait on 2,000 objects of four trees |

Lua has no JSON in its standard library, so `jsondoc` has no Lua version.
`+=` on a string copies it in CPython and Lua, so the `textbuild` loop is
kept short; Plumlang adds to a string in place when the variable is its
only owner (#148).

```bash
make                                   # release build, -O2
python3 bench/run.py                   # ./plum against everything installed
python3 bench/run.py --runs 7 --only words,cart
python3 bench/run.py --lua luajit      # a particular Lua
python3 bench/run.py --json out.json   # also write the results as JSON
```

Results depend on the machine; compare runs on the same one. The release
binaries are built with `make release-pgo` (LTO and PGO, below); build
that to compare with what users run.

## Results

Linux x86_64, Intel Core i5-13450HX, Plumlang built with GCC 15.2 at
`-O2`; Python 3.14.4, Lua 5.3 (LuaTeX 1.22's `texlua`), Node.js 24.21;
fastest of five runs, on a machine that was busy with other work. The
"x" columns are how many times longer the other language took: above 1,
Plumlang is faster.

| Benchmark | plum | python3 | | lua | | node | |
|---|---|---|---|---|---|---|---|
| cart | 0.307 s | 0.498 s | 1.62x | 0.332 s | 1.08x | 0.038 s | 0.12x |
| collatz | 0.581 s | 2.278 s | 3.92x | 0.759 s | 1.31x | 0.246 s | 0.42x |
| dispatch | 0.264 s | 0.283 s | 1.07x | 0.264 s | 1.00x | 0.033 s | 0.13x |
| fib | 0.049 s | 0.059 s | 1.19x | 0.049 s | 1.00x | 0.025 s | 0.50x |
| jsondoc | 0.239 s | 0.236 s | 0.99x | - | | 0.119 s | 0.50x |
| loop | 0.199 s | 0.702 s | 3.52x | 0.189 s | 0.95x | 0.048 s | 0.24x |
| mandelbrot | 0.159 s | 0.876 s | 5.49x | 0.196 s | 1.23x | 0.033 s | 0.21x |
| strings | 0.142 s | 0.167 s | 1.17x | 0.128 s | 0.90x | 0.046 s | 0.32x |
| textbuild | 0.046 s | 0.312 s | 6.71x | 0.307 s | 6.62x | 0.057 s | 1.23x |
| words | 0.274 s | 0.310 s | 1.13x | 0.188 s | 0.69x | 0.261 s | 0.95x |

(`textbuild` was measured again after #148, fastest of five runs.)
Plumlang is ahead of CPython or within 1% of it on every benchmark, and
3.5 to 5.5 times faster on integer and float arithmetic (`loop`,
`collatz`, `mandelbrot`). It is level with Lua's interpreter on calls
(`fib`, `dispatch`), ahead of it on `collatz`, `mandelbrot` and `cart`,
and behind it by up to 1.46 times on `words`, where the time goes into
hashing and allocating strings. Node.js compiles hot code to machine code
(V8's JIT), which an interpreter doesn't; `words` is where it is closest.

## In CI

`.github/workflows/bench.yml` runs on every pull request and every push
to `main`, on `ubuntu-latest`:

1. builds the release binary (`make`) and installs Lua 5.4 (Python and
   Node.js come with the runner);
2. runs `bench/run.py --runs 5 --json bench-results/results.json`; a
   program that fails or prints the wrong output fails the job;
3. uploads `results.json` as the `bench-results` artifact (kept 90 days);
4. downloads `bench-results` from the last successful `push` run on `main`
   (`gh run list` / `gh run download`);
5. runs `bench/compare.py`, which writes a table to the job summary and,
   on a pull request, fails if a benchmark's Plumlang time is more than
   15% **and** more than 0.02 s slower than `main`'s. The 0.02 s keeps a
   very short benchmark's noise from failing it alone. On `main` the
   comparison is only reported, so each push becomes the next baseline.
   With no baseline (the first run, or an expired artifact) it passes.

Python, Lua and Node.js times are in the table for reference and never
fail the check. Runners vary from run to run, which is why the threshold
is 15%; to change it, set the repository variable `BENCH_THRESHOLD` (a
percentage) under Settings, Secrets and variables, Actions, Variables. To
check a change locally against a saved result:

```bash
python3 bench/run.py --runs 5 --json before.json     # on main
python3 bench/run.py --runs 5 --json after.json      # with the change
python3 bench/compare.py after.json before.json --threshold 10
```

## Idle connections

`bench/net/idle.plum` opens 10,000 TCP connections to its own server, a
task per connection on the server side, all waiting to read, then times a
new connection made while they idle. Linux x86_64, GCC 15.2 `-O2`, with
`ulimit -n 30000`:

| | Result |
|---|---|
| 10,000 connections open and idle | 0.16 s |
| a new connection while they idle | 0.2 ms |
| memory, all told | 84 MB (both ends of every connection, in one process) |

Before the scheduler used epoll and a timer heap (each wait scanned every
task and polled every socket), opening them took 6.0 s and a new
connection 4.4 ms.

## HTTP

`python3 bench/net/http_load.py` starts `bench/net/http_server.plum` and
sends requests back to back from 8 client processes, each keeping one
HTTP/1.1 connection alive, for 4 seconds:

| Route | Requests / s | p50 | p99 |
|---|---|---|---|
| `/hello/:name` (text) | 126,832 | 0.06 ms | 0.12 ms |
| `/api/item/:id` (JSON) | 112,305 | 0.07 ms | 0.16 ms |

The server is one thread running `net.http`, which is written in
Plumlang. A count kept by the server matched the client's (41,574 of
41,574). One Python client manages about 20,000 requests a second, so
these numbers are partly the clients'.

## What was tried

Each change was measured and kept only if it helped. The rows up to
computed-goto dispatch were measured with `bench/run.py --runs 7`; the
LTO and PGO rows with `--runs 5` (Linux x86_64, GCC 15.2); the rows for
typed operands (#118) with the best of several runs of each benchmark
during development, and their totals below with
`bench/run.py --runs 5 --plum-only`, on the machine above.

| Change | Result | Kept |
|---|---|---|
| Typed `==` / `!=` for numbers (`EQ_INT`, `EQ_FLOAT`) instead of a generic comparison | collatz 1.41 s → 1.27 s, loop 0.30 s → 0.28 s | yes |
| Computed-goto dispatch (GCC/Clang) instead of `switch` | no difference (collatz 1.27 s → 1.32 s, within noise): GCC's jump table is already well predicted | no |
| Compiler-computed stack bound, so `push` skips its capacity check | no difference: the check is a perfectly predicted branch | no |
| Link-time optimisation alone (`-flto`, `make release-lto`) | mixed: jsondoc 0.236 s → 0.192 s and strings 0.150 s → 0.136 s (the stdlib's C inlined into the VM's calls), but cart, dispatch and loop 3–5% slower | only as part of PGO, and where PGO can't run |
| Profile-guided optimisation alone (`-fprofile-use`, no LTO) | collatz −11%, fib −9%, jsondoc −9%, strings −8%; dispatch and cart within noise | no: LTO + PGO does more |
| LTO + PGO (`make release-pgo`, trained by `tools/pgo-train.sh` on bench/ and examples/) | cart −17%, dispatch −17%, collatz −12%, fib −12%, jsondoc −11%, words −11%, strings −9%; loop, mandelbrot and textbuild unchanged; nothing slower | yes, for release binaries |
| `plum_release` inline everywhere | slower: loop 0.27 s → 0.33 s, collatz 1.29 s → 1.36 s; the run loop grew and spilled more registers | no |
| Typed arithmetic in place on the stack (no pop, pop, push), overflow checks with `__builtin_*_overflow` where the compiler has it, the step-limit check inline | collatz 1.30 s → 1.26 s, mandelbrot 0.34 s → 0.33 s | yes |
| Right operand as an operand of the instruction (`_K` constant, `_L` local); `FOR_NEXT` jumps back to the body, not to `FOR_RANGE` | collatz 1.26 s → 1.16 s, mandelbrot 0.33 s → 0.24 s, loop 0.26 s → 0.25 s | yes |
| Compare and jump in one instruction for `if` / `while` conditions and `and` chains (`JUMP_IF_NOT_LT_INT`) | collatz 1.16 s → 1.10 s, loop 0.25 s → 0.24 s | yes |
| `GET_LOCAL` / `SET_LOCAL` (and globals) copy a value's type and number separately, and release only objects | collatz 1.10 s → 0.91 s, mandelbrot 0.24 s → 0.19 s: an in-place instruction writes only the number (8 bytes), and reading the whole 16-byte value straight after stalled on a failed store-to-load forward | yes |
| Compare with a literal and jump (`JUMP_IF_NOT_NE_INT_K`) | collatz 0.91 s → 0.86 s, mandelbrot 0.19 s → 0.18 s | yes |
| `//` and `%` in 32 bits when both operands are small and positive, a mask for `%` by a power of two | collatz 0.86 s → 0.73 s, loop 0.23 s → 0.20 s | yes |
| Change an int local in place (`LOCAL_ADD_INT_K` for `n += 1`, `LOCAL_FLOORDIV_INT_K` for `n = n // 2`) | collatz 0.73 s → 0.63 s, mandelbrot 0.18 s → 0.16 s | yes |
| Local op literal pushed by one instruction (`MOD_INT_LK`); `+=` / `-=` on an int global in place (`GLOBAL_ADD_INT_L`) | collatz 0.63 s → 0.58 s; loop unchanged at 0.20 s although it runs 3.3 instructions a time round instead of 5 | yes |
| The running frame's slot base and constant array kept in local variables | no difference: they are spilled to the stack like the frame pointer | no |
| Computed-goto dispatch again, with the typed operands in | within noise: loop 0.20 s → 0.19 s, mandelbrot 0.16 s → 0.15 s, dispatch 0.26 s → 0.27 s, cart 0.30 s → 0.31 s | no |
| `+=` on a str variable adds to its buffer in place when the variable is its only owner and the string isn't frozen, the buffer growing by half each time (`APPEND_LOCAL`, `APPEND_GLOBAL`, #148); a chain of str `+` joined by one `BUILD_STRING` | textbuild 0.325 s → 0.051 s (`--runs 7`); with 200,000 lines instead of 20,000, 57.4 s (one run) → 0.46 s (fastest of seven), where CPython 3.14 takes 50.4 s; the rest within noise (table below) | yes |

LTO and PGO, Plumlang alone (`--plum-only`), fastest of five runs, the
four builds measured one after another:

| Build | cart | collatz | dispatch | fib | jsondoc | loop | mandelbrot | strings | textbuild | words |
|---|---|---|---|---|---|---|---|---|---|---|
| `make` (`-O2`) | 0.397 s | 1.302 s | 0.307 s | 0.064 s | 0.236 s | 0.273 s | 0.337 s | 0.150 s | 0.320 s | 0.302 s |
| `make release-lto` | 0.411 s | 1.318 s | 0.319 s | 0.062 s | 0.192 s | 0.288 s | 0.347 s | 0.136 s | 0.320 s | 0.289 s |
| PGO without LTO | 0.384 s | 1.165 s | 0.302 s | 0.058 s | 0.214 s | 0.269 s | 0.347 s | 0.138 s | 0.322 s | 0.297 s |
| `make release-pgo` (LTO + PGO) | 0.328 s | 1.140 s | 0.255 s | 0.056 s | 0.211 s | 0.271 s | 0.341 s | 0.136 s | 0.320 s | 0.270 s |

PGO lays the run loop out for the paths the training ran; LTO lets it also
inline hot calls across files, which is where the method-dispatch gain
(`dispatch`, `cart`) comes from: PGO alone leaves those as they were.
`loop`, `mandelbrot` and `textbuild` spend their time in a few
instructions that `-O2` already lays out well, or in `memcpy`.

Release builds use it where the build machine can run the training:
Linux x86_64 and arm64 (GCC; Clang on Linux passes the whole suite too).
Linux riscv64 (built under emulation) gets LTO only. macOS arm64 gets
LTO + PGO too (Apple Clang), and x86_64, cross-compiled so it can't be
trained, LTO alone. Until #173, macOS releases had no LTO: every Apple
Clang LTO build aborted in the network tests (`pointer being freed was
not allocated`, the "pointer" being the bytes `GET /hel`). The cause was
a name: src/lsp/server.c had a `static void send(...)`, and macOS's
`<sys/socket.h>` declares `send()` with an assembler name
(`__asm("_send")`). Once LTO merges the files into one module, the
static function and the library's `send` are the same symbol, `_send`,
and net.c's `send(fd, text, len, flags)` called the language server's
function, which took the text for a JSON writer and freed its first
eight bytes. Two files reproduce it with Apple Clang 17 and 21, full or
thin LTO; Linux builds keep the two apart:

```c
/* a.c */
#include <stdio.h>
void call_send(int fd);
__attribute__((noinline)) static void send(const char *who) { printf("static send (%s)\n", who); }
void lsp(void) { send("lsp"); send("lsp again"); }
int main(void) { lsp(); call_send(-1); return 0; }

/* b.c */
#include <stdio.h>
#include <sys/socket.h>
void call_send(int fd) { printf("libc send returned %ld\n", (long)send(fd, "GET /hello", 10, 0)); }
```

`clang -O2 a.c b.c` prints `libc send returned -1`; `clang -O2 -flto
a.c b.c` crashes in the static `send`. The function is now
`send_message`, and `tests/symbols.sh` (part of `make test`) fails if
any function of ours, static ones included, has the name of one the
binary imports from the system. Windows gets
MSVC's LTCG (`-DPLUM_LTO=ON`) and no PGO, as MSVC's needs its own runtime
DLL while training and the ARM64 build is cross-compiled.

Before and after the typed operands (#118), Plumlang alone:

| Benchmark | before | after | |
|---|---|---|---|
| cart | 0.399 s | 0.310 s | 1.29x |
| collatz | 1.300 s | 0.580 s | 2.24x |
| dispatch | 0.307 s | 0.268 s | 1.15x |
| fib | 0.063 s | 0.049 s | 1.29x |
| jsondoc | 0.241 s | 0.241 s | 1.00x |
| loop | 0.274 s | 0.200 s | 1.37x |
| mandelbrot | 0.340 s | 0.160 s | 2.13x |
| strings | 0.148 s | 0.143 s | 1.03x |
| textbuild | 0.328 s | 0.325 s | 1.01x |
| words | 0.310 s | 0.281 s | 1.10x |

#118 asked for `loop`, `collatz` and `mandelbrot` at least twice as fast:
`collatz` and `mandelbrot` are, `loop` is 1.37 times. Its loop body is now
three instructions (`MOD_INT_LK`, `JUMP_IF_NOT_EQ_INT_K`, `FOR_NEXT`, and
`GLOBAL_ADD_INT_L` a third of the time), each costing about 30 machine
instructions, with no branch mispredicted. What is left is the
bookkeeping of a stack machine run by one large function: the stack top
lives in memory rather than in a register, and the frame and constants
are reloaded from the stack for every instruction. Keeping them in
registers, a register-based instruction set (#123) or 8-byte values
(#121) are the next steps, measured with this suite.

Method calls cost what function calls do (`dispatch` and `fib` both run
level with Lua): finding the method is one table lookup, and the time
goes into setting up each call's frame.

Before and after appending in place (#148), Plumlang alone,
`bench/run.py --runs 7 --plum-only`, the two builds measured one after
the other:

| Benchmark | before | after |
|---|---|---|
| cart | 0.309 s | 0.316 s |
| collatz | 0.585 s | 0.596 s |
| dispatch | 0.269 s | 0.273 s |
| fib | 0.050 s | 0.049 s |
| jsondoc | 0.237 s | 0.239 s |
| loop | 0.201 s | 0.201 s |
| mandelbrot | 0.161 s | 0.162 s |
| strings | 0.143 s | 0.143 s |
| textbuild | 0.325 s | 0.051 s |
| words | 0.283 s | 0.276 s |

Measured again in turns, `cart`, `collatz` and `dispatch` moved by up to
3% either way between the two builds, which is this machine's noise. The
loop that adds a line costs one `APPEND_GLOBAL` and the instructions of
the interpolation; what is left of `textbuild` is mostly the `join` half.
