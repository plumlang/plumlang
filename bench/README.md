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
| `lists` | collections handed to functions: a 200,000-int list summed and multiplied by index, a 300 x 300 grid read by column, 100,000 words grouped in a `map[str, list[str]]`, and a list built by a helper that returns it |

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
| cart | 0.142 s | 0.505 s | 3.55x | 0.339 s | 2.39x | 0.039 s | 0.28x |
| collatz | 0.583 s | 2.361 s | 4.05x | 0.768 s | 1.32x | 0.250 s | 0.43x |
| dispatch | 0.089 s | 0.286 s | 3.20x | 0.268 s | 3.00x | 0.034 s | 0.38x |
| fib | 0.046 s | 0.059 s | 1.29x | 0.050 s | 1.08x | 0.026 s | 0.58x |
| jsondoc | 0.250 s | 0.248 s | 0.99x | - | | 0.128 s | 0.51x |
| loop | 0.206 s | 0.750 s | 3.64x | 0.192 s | 0.93x | 0.050 s | 0.25x |
| mandelbrot | 0.155 s | 0.887 s | 5.73x | 0.200 s | 1.29x | 0.035 s | 0.23x |
| strings | 0.149 s | 0.167 s | 1.12x | 0.130 s | 0.87x | 0.050 s | 0.34x |
| textbuild | 0.046 s | 0.312 s | 6.71x | 0.307 s | 6.62x | 0.057 s | 1.23x |
| words | 0.287 s | 0.317 s | 1.10x | 0.190 s | 0.66x | 0.263 s | 0.92x |

(`textbuild` was measured after #148, the rest after #119, fastest of
five runs each.)

The table predates the cheaper calls of #120: `fib`, `cart`, `collatz`
and `mandelbrot` are faster now (see "Calls (#120)" below).
Plumlang is ahead of CPython or within 1% of it on every benchmark, and
3.5 to 5.7 times faster on integer and float arithmetic (`loop`,
`collatz`, `mandelbrot`) and on method calls (`dispatch`, `cart`). It is
three times as fast as Lua's interpreter on method calls (`dispatch`) and
2.4 times on `cart`, level with it on function calls (`fib`), ahead on
`collatz` and `mandelbrot`, and behind it by up to 1.5 times on `words`,
where the time goes into hashing and allocating strings. Node.js compiles
hot code to machine code (V8's JIT), which an interpreter doesn't; `words`
is where it is closest.

## In CI

`.github/workflows/bench.yml` runs on every pull request and every push
to `main`, on `ubuntu-latest`:

1. builds the release binary (`make`) and installs Lua 5.4 (Python and
   Node.js come with the runner);
2. on a pull request, also builds `main` in a worktree: the first parent
   of the merge commit GitHub tests, so `main` as the pull request would
   merge into it;
3. runs `bench/run.py --json bench-results/results.json`, on a pull
   request with `--baseline` main's `plum`: each benchmark runs the two
   binaries in turn, 7 times each (swapping which goes first every pair),
   and takes each one's median. A program that fails or prints the wrong
   output fails the job;
4. uploads `results.json`, every run's time included, as the
   `bench-results` artifact (kept 90 days);
5. runs `bench/compare.py`, which writes a table to the job summary and,
   on a pull request, fails if a benchmark's Plumlang median is more than
   15% **and** more than 0.02 s slower than `main`'s. The 0.02 s keeps a
   very short benchmark's noise from failing it alone. On `main` the
   times are only recorded.

Both binaries are timed on the same runner, interleaved, so a slow or
busy runner slows both alike; comparing with an earlier run on another
runner (as the job did before #172) failed pull requests that didn't
touch the VM by 15-47%. Python, Lua and Node.js times (fastest of 5) are
in the table for reference and never fail the check. To change the
threshold, set the repository variable `BENCH_THRESHOLD` (a percentage)
under Settings, Secrets and variables, Actions, Variables. To compare a
change locally with `main` the same way:

```bash
git worktree add ../plum-main origin/main && make -C ../plum-main
make
python3 bench/run.py --baseline ../plum-main/plum --json after.json
python3 bench/compare.py after.json --threshold 10
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
`bench/run.py --runs 5 --plum-only`, on the machine above. The rows for
fields and method calls (#119) are single runs of `make` builds of each
step, or cycles from `perf stat -r 5` where the step was too small for
one run to show; their totals are below.

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
| Fields of an object in a local by slot (`GET_FIELD_L`, `SET_FIELD_L`), `+=` / `-=` on an int or float field in place (`FIELD_ADD_INT_K`, ...), and direct calls of methods no tree overrides (`CALL_METHOD`) (#119) | dispatch 0.30 s → 0.155 s, cart 0.335 s → 0.238 s: `Tally.add` is 3 instructions instead of 15, and `self.r` no longer counts a reference to `self` | yes |
| `INVOKE` shifting the arguments with a loop instead of `memmove` | no difference: GCC turns the loop back into a `memmove` call | no |
| A return dropping references inline while something else still holds the object; a new frame's locals set without a capacity check each | dispatch 0.155 s → 0.128 s, cart 0.238 s → 0.209 s | yes |
| A method's frame without the function under the object: slot 0 is the object, and the result takes its place, so nothing is shifted | dispatch 0.128 s → 0.117 s, cart 0.209 s → 0.189 s | yes |
| The usual method call (no limits, room on the stack, the frame segment there) inline in the run loop | dispatch 0.117 s → 0.103 s, cart 0.189 s → 0.168 s | yes |
| A return reading the result and the locals a field at a time (the result was often just written in place) | cart −2% in cycles (`perf stat -r 5`), dispatch −2% | yes |
| `FOR_EACH` testing for a list first, and dropping the item before inline | cart −6% in cycles | yes |
| `INVOKE` and `CALL_METHOD` entering the new frame they made, instead of finding it again | cart −5% in cycles, dispatch −8% | yes |
| A method call that is a statement dropping its result on return, with no `POP` after it | cart −2%, dispatch unchanged | no: a flag in every frame and call for little |
| A per-call-site cache of the method found for the last tree | not needed: `INVOKE` already finds the method with one array index (the method's number in the object's tree's table), and a cache written while running would have to live outside the bytecode, which isolates share (#150) | no |

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
eight bytes. Two files reproduce it with Apple Clang 17 and 21 and
upstream LLVM 23, full or thin LTO, for arm64 and x86_64; on Linux the
two stay apart, even when the library function has an assembler name:

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
binary imports from the system.

macOS arm64 (GitHub's macos-latest runner, Apple Clang 21), fastest of
four rounds of `--runs 5 --plum-only`, the builds taking turns (before
#148 made textbuild faster); a shared
runner, so differences under about 10% are noise:

| Build | cart | collatz | dispatch | fib | jsondoc | loop | mandelbrot | strings | textbuild | words |
|---|---|---|---|---|---|---|---|---|---|---|
| `make` (`-O2`) | 0.530 s | 1.003 s | 0.378 s | 0.099 s | 0.325 s | 0.220 s | 0.271 s | 0.313 s | 0.154 s | 0.397 s |
| PGO without LTO (before #173) | 0.534 s | 1.026 s | 0.432 s | 0.082 s | 0.294 s | 0.184 s | 0.327 s | 0.283 s | 0.173 s | 0.284 s |
| `make release-pgo` (LTO + PGO) | 0.462 s | 0.960 s | 0.360 s | 0.084 s | 0.309 s | 0.193 s | 0.296 s | 0.283 s | 0.140 s | 0.312 s |

Against PGO alone, LTO + PGO makes method dispatch faster here too
(cart −13%, dispatch −17%) and textbuild −19%; the rest is within the
noise.

Windows gets MSVC's LTCG (`-DPLUM_LTO=ON`) and no PGO, as MSVC's needs
its own runtime DLL while training and the ARM64 build is cross-compiled.

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

## Calls (#120)

`perf record` of `fib(32)` on a `-O2 -g` build, before: 80% of the time
in the run loop, 9% in `call()` and 8% in `plum_release`, about 370
machine instructions and 82 cycles per Plumlang call. By source line, the
time went to the dispatch switch (~14%), finding the frame's segment
(`FRAME()`, a signed division: ~11%), releasing the slots and the
function value on return (a call to `plum_release` per value: ~15%),
`call()` itself (the step and recursion checks, a new segment, `grow`, a
`push` per slot: ~17%), and `GET_GLOBAL` of the function (its unset
check and reference count, released again on return). Branch
mispredictions were negligible (0.1%): the cost was instructions, at 3.8
per cycle.

What changed, each measured with `perf stat` on `fib(32)` (cycles, best
of five, pinned to one core). The first rows were measured on main before
#119, the last ones after rebasing on it:

| Change | fib(32) cycles | Kept |
|---|---|---|
| before | 579 M | |
| Frames without a closure pointer (a closure's captures are read through the function value under slot 0), `CALL_GLOBAL` for a declared function called by name (not pushed, no reference counted), methods called without moving their arguments, the rare call checks in one test, an inline return | 433 M | yes |
| `JUMP_IF_NOT_*_INT_LK` (an int local against a literal) and `RETURN_LOCAL` | 292 M | yes |
| Releasing a returning call's slots: a scan without calls first, so a frame of numbers spills no registers | 282 M | yes |
| `CALL_SELF`: a function calling itself reads itself from its frame | 238 M | yes |
| Threaded dispatch (computed goto) for the eight hottest instructions | 233 M; 243 M with `-fno-gcse -fno-crossjumping`, which GCC needs to keep the jumps apart; tried again after #119, `collatz` got 20% slower | no |
| A switch over all 256 byte values, to drop its range check | 243 M: GCC keeps the check | no |
| `RETURN_ADD_INT` (`return a + b` of ints), computed without a round trip through the stack | 213 M | yes |
| main after #119 (fixed-offset fields, static method dispatch), before this change | 513 M | |
| this change rebased on #119 | 233 M | |
| The `_LK` instructions hold their literal (-32768 to 32767) as the operand: no constant to load | 217 M | yes |
| The slot base in a local, frames carrying their constants, one compare (`call_room`) for whether a call takes the fast way | 2–3% fewer cycles with GCC 15, but with GCC 13 (what CI builds with) `dispatch` 19% and `loop` 50% slower | no |
| `hot` attributes on the call and return labels | no difference | no |

Tail calls (`return f(...)` of a function's own call, `TAIL_SELF`) and
inlining small functions that can't fail don't change these benchmarks;
they let such code run without the recursion limit, or without a call
(spec 6.5). A call now costs about 155 instructions and 31 cycles
(before: 313 and 73 on main after #119). 75% of the machine's issue slots
retire work, so what is left is instructions: each one reads and writes
`vm->top` in memory, GCC keeps the running frame on the stack, and every
argument is stored and read back once. Keeping the stack top in a
register or a register VM (#123) would remove those.

Before and after, Plumlang alone, `make` (`-O2`, GCC 15), best of three
alternating runs of `bench/run.py --runs 5 --plum-only` pinned to one
core, before being main with #119:

| Benchmark | before | after | |
|---|---|---|---|
| cart | 0.142 s | 0.122 s | 1.16x |
| collatz | 0.573 s | 0.449 s | 1.28x |
| dispatch | 0.088 s | 0.090 s | 0.98x |
| fib | 0.044 s | 0.019 s | 2.32x |
| jsondoc | 0.232 s | 0.233 s | 1.00x |
| loop | 0.204 s | 0.199 s | 1.03x |
| mandelbrot | 0.154 s | 0.139 s | 1.11x |
| strings | 0.140 s | 0.141 s | 0.99x |
| textbuild | 0.044 s | 0.043 s | 1.02x |
| words | 0.257 s | 0.260 s | 0.99x |

Counting cycles (`perf stat`, best of five) is steadier. With GCC 15,
`-O2`: fib −58%, collatz −21%, cart −17%, mandelbrot −9%, loop −2%,
dispatch −2%, textbuild −2%; `strings`, `words` and `jsondoc` +2.1%,
+0.6% and +0.3%, which run none of the changed instructions: GCC laying
out the large run loop differently. With GCC 13: fib −45%, loop −25%,
cart −19%, collatz −18%, dispatch −10%, strings and words level. With
`make release-pgo` (GCC 15, before the last change to how a return drops
references): fib −60%, cart −16%, collatz −9%, strings −6%, dispatch,
jsondoc and loop −1%, the rest within 0.3%.

With `python3 bench/run.py --runs 7` on the same machine, pinned to one
core, `fib` takes 0.018 s to 0.019 s against CPython 3.14's 0.057 s:
3.07 to 3.09 times as fast (1.36 times before). Node.js takes 0.024 s.

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

Before and after fixed-offset fields and static method dispatch (#119),
on top of #148, Plumlang alone, `make` (`-O2`), fastest of 14 runs
(`--runs 7`, twice, the two builds in turns). The machine was busy with
other work and ran slower than for the tables above, so compare the
ratios:

| Benchmark | before | after | |
|---|---|---|---|
| cart | 0.492 s | 0.226 s | 2.18x |
| collatz | 0.917 s | 0.908 s | 1.01x |
| dispatch | 0.429 s | 0.139 s | 3.09x |
| fib | 0.077 s | 0.069 s | 1.11x |
| jsondoc | 0.342 s | 0.347 s | 0.99x |
| loop | 0.314 s | 0.323 s | 0.97x |
| mandelbrot | 0.253 s | 0.244 s | 1.04x |
| strings | 0.221 s | 0.224 s | 0.98x |
| textbuild | 0.068 s | 0.069 s | 0.99x |
| words | 0.391 s | 0.388 s | 1.01x |

`loop` and `strings` run no method calls: the 3% is where GCC placed the
run loop's code, which moved as it grew (their instruction counts are
unchanged or lower; in cycles, `perf stat -r 7`, `loop` is 2.7% and
`strings` 1% slower, `textbuild`, `words` and `jsondoc` level). With
`make release-pgo` (LTO and PGO, `--runs 7`, measured before #148, on a
quieter machine), nothing is slower:

| Benchmark | before | after | |
|---|---|---|---|
| cart | 0.244 s | 0.140 s | 1.75x |
| collatz | 0.452 s | 0.446 s | 1.01x |
| dispatch | 0.214 s | 0.084 s | 2.54x |
| fib | 0.042 s | 0.041 s | 1.04x |
| jsondoc | 0.196 s | 0.195 s | 1.01x |
| loop | 0.165 s | 0.165 s | 1.00x |
| mandelbrot | 0.137 s | 0.137 s | 1.00x |
| strings | 0.128 s | 0.125 s | 1.02x |
| textbuild | 0.321 s | 0.319 s | 1.01x |
| words | 0.248 s | 0.242 s | 1.02x |

PGO had already sped up the old method calls (LTO inlined the call into
the run loop), so the gain is smaller there for `cart`; the new code runs
as fast with or without it.

#119 asked for `dispatch` at least twice as fast as 2.2.0: it is 3.1
times with `make` and 2.5 times with the release build. A method call is
now cheaper than a function call: the object is slot 0 where the caller
pushed it, a field of `self` is read without counting a reference, and
`self.count += 1` is one instruction. What is left in `cart` is the run
loop's own cost, about 20 instructions dispatched per item.

## Value layout (#121)

A value was a 16-byte tagged union: a 4-byte type, padding, and an 8-byte
number or pointer. #121 asked whether 8-byte NaN-boxed values are faster.
Both layouts are now built from the same source (`src/vm/value.h`; the
rest of the code reads and makes values through its functions), chosen
with `make NANBOX=1` or `cmake -DPLUM_NANBOX=1`. The NaN-boxed layout:

| Bits (hex) | Value |
|---|---|
| below `FFF9 0000 0000 0000` | a float, its own IEEE 754 bits (`-nan` is `FFF8 0000 0000 0000`) |
| `FFF9 0000 0000 000x` | none (x = 0), the unset module variable (1), `bad` (2), `good` (3) |
| `FFFA pppp pppp pppp` | a heap object at the 48-bit address p |
| `FFFC ...` to `FFFF ...` | an int from -2^49 to 2^49 - 1, stored as i + `FFFE 0000 0000 0000` |

**Ints stay 64-bit.** An int outside ±2^49 (about ±5.6 × 10^14) is
*boxed*: a small immutable heap object (`PlumBigInt`, `PLUM_OBJ_INT`)
behind the object tag, reference-counted like any other. An int that fits
inline is never boxed, so equal inline ints have equal bits. Every int
instruction has a fast path, taken when both operands and the result are
inline (always, in the 16-byte layout). Otherwise it goes to a slow path
out of the run loop (`int_arith_slow`, `int_compare_slow`), which computes
in 64 bits, raises the same `OverflowError` with the same message at the
64-bit edge, and boxes or unboxes. The cost of the trade-off: an int
beyond ±2^49 (nanosecond timestamps, hashes, `1 << 60`) costs an
allocation each time one is made, and a pointer must fit in 48 bits
(`plum_obj` stops the program with a message if one doesn't: no
platform Plumlang builds for hands out such addresses). The alternative,
51-bit ints with no boxing, would change the language (`int` is 64-bit
with overflow errors, spec 4.4), so it was not considered.

**NaNs.** A NaN made by hardware or `strtod` can carry any payload and
could land among the tagged values, which would be a memory-safety hole.
`plum_float` turns every NaN into the payload-less one of its sign, which
also keeps the sign that a `:f` format prints. The VM's `+ - * /` store
their result unchanged (`plum_float_raw`), since IEEE 754 arithmetic on
such NaNs gives one of the same two (and so does WebAssembly, whose rule
for canonical NaNs is the same).

**Isolates (#150).** Boxed ints are ordinary objects, one per value, with
no process-wide table. The object header and its 32-bit count are
unchanged, the bytecode is untouched, and nothing is kept on the C stack.

`tests/lang/arith/big_ints.plum` runs every kind of int instruction
across ±2^49 (stack, `_K`, `_L`, `_LK`, locals, module variables, fields,
ranges, compare-and-jump, `return a + b`). It keeps boxed ints in lists,
maps, sets, objects, closures, tasks and channels, and covers sorting,
`compare()`, formatting, JSON and the overflow and index errors. Its
expected output is the 16-byte build's; the NaN-boxed build prints the
same. The whole suite passes in both layouts. CI builds and tests the
NaN-boxed layout under ASan and UBSan (GCC and Clang), on Linux arm64,
on macOS arm64, and as WebAssembly (a 32-bit target), so it keeps
working.

### Measurements

Linux x86_64, Intel Core i5-13450HX, GCC 15.2. Times are medians of 11
pairs from `bench/run.py --baseline --plum-only --pairs 11`, each build
taking turns with main (#120 merged) on one core (`taskset -c 3`).
Instructions and cycles come from `perf stat -r 3` on the same core.
"16 bytes" is this change in the default layout; "8 bytes" is
`NANBOX=1`. The % column is 8 bytes against main. The first four time
columns are `make` (`-O2`), the last four `make release-pgo`:

| Benchmark | `make`: main | 16 bytes | 8 bytes | | PGO: main | 16 bytes | 8 bytes | |
|---|---|---|---|---|---|---|---|---|
| cart | 0.132 s | 0.126 s | 0.144 s | +9% | 0.118 s | 0.123 s | 0.129 s | +9% |
| collatz | 0.465 s | 0.452 s | 0.514 s | +10% | 0.419 s | 0.438 s | 0.355 s | -15% |
| dispatch | 0.092 s | 0.091 s | 0.093 s | +1% | 0.083 s | 0.084 s | 0.082 s | -1% |
| fib | 0.021 s | 0.020 s | 0.024 s | +15% | 0.018 s | 0.020 s | 0.019 s | +7% |
| jsondoc | 0.243 s | 0.234 s | 0.233 s | -4% | 0.193 s | 0.191 s | 0.180 s | -6% |
| loop | 0.203 s | 0.200 s | 0.224 s | +10% | 0.162 s | 0.162 s | 0.101 s | -38% |
| mandelbrot | 0.150 s | 0.147 s | 0.169 s | +13% | 0.147 s | 0.166 s | 0.136 s | -7% |
| strings | 0.149 s | 0.149 s | 0.150 s | +1% | 0.120 s | 0.123 s | 0.128 s | +7% |
| textbuild | 0.044 s | 0.044 s | 0.042 s | -5% | 0.037 s | 0.037 s | 0.037 s | -0% |
| words | 0.266 s | 0.263 s | 0.265 s | -0% | 0.229 s | 0.233 s | 0.235 s | +2% |

Geometric mean against main: with `make`, 16 bytes 0.997, 8 bytes 1.048
(5% slower); with `make release-pgo`, 16 bytes 1.038, 8 bytes 0.947 (5%
faster).

The two builds of the 16-byte layout are the same code (the accessors
compile to the same instructions), so their PGO difference measures the
noise of code layout alone. On `mandelbrot`, main's and this change's
PGO builds run 3.038 G and 3.033 G instructions but take 626 M and
718 M cycles (+15%). The run loop starts 32 bytes further along in one
of them. Any single PGO time here can move about 15% that way. The
instructions retired don't depend on layout (`make` unless marked PGO):

| Benchmark | instructions: 16 bytes | 8 bytes | | cycles: 16 bytes | 8 bytes | | PGO cycles: main | 8 bytes | |
|---|---|---|---|---|---|---|---|---|---|
| cart | 2.64 G | 2.81 G | +6% | 538 M | 597 M | +11% | 510 M | 564 M | +11% |
| collatz | 6.02 G | 7.33 G | +22% | 1920 M | 2238 M | +17% | 1862 M | 1566 M | -16% |
| dispatch | 1.91 G | 1.88 G | -1% | 392 M | 397 M | +1% | 361 M | 361 M | -0% |
| fib | 0.42 G | 0.45 G | +8% | 85 M | 100 M | +16% | 75 M | 81 M | +8% |
| jsondoc | 4.00 G | 4.00 G | +0% | 1044 M | 1009 M | -3% | 862 M | 796 M | -8% |
| loop | 1.94 G | 2.40 G | +24% | 905 M | 1018 M | +12% | 737 M | 443 M | -40% |
| mandelbrot | 3.02 G | 2.68 G | -11% | 640 M | 742 M | +16% | 626 M | 585 M | -6% |
| strings | 2.68 G | 2.72 G | +1% | 667 M | 659 M | -1% | 526 M | 568 M | +8% |
| textbuild | 0.72 G | 0.72 G | -0% | 199 M | 185 M | -7% | 165 M | 164 M | -0% |
| words | 4.03 G | 4.10 G | +2% | 1160 M | 1159 M | -0% | 1006 M | 1030 M | +2% |

What the counts show:

- An inline int costs more to use than a 16-byte one. Each int
  instruction checks that its operands are inline (an `and` and a
  compare against a 64-bit constant), decodes them, checks the result's
  range and encodes it. That is +22% instructions on `collatz`, +24% on
  `loop`, +8% on `fib` (its `n - 1` and `n < 2`) and +6% on `cart`.
- A float is cheaper: no type word to write, so `mandelbrot` runs 11%
  fewer instructions.
- Calls and references cost about the same. "Is this an object" is a
  shift and a compare on the loaded word instead of a compare on the
  type word, and copying a value is one 8-byte move instead of two.
- With `make`, the 8-byte build is front-end bound where the 16-byte one
  isn't: 43% of `mandelbrot`'s issue slots on fetch, against 2%. Its run
  loop is larger (39 KB against 32 KB, after moving the slow paths out;
  50 KB with them in), and its 64-bit constants are 10-byte
  instructions. PGO lays the loop out by profile, which removes most of
  that, and the 8-byte build is then ahead on `loop`, `collatz`,
  `mandelbrot` and `jsondoc`.
- `loop` is backend-bound in both layouts at `-O2` (64% of slots):
  every instruction reads and writes `vm->top` in memory. Whether the
  processor forwards that well depends on where the code lands. One
  8-byte build ran `loop` at 412 M cycles and another, after an
  unrelated change, at 1011 M, with the same instructions. Its −40%
  under PGO is that effect, not the value size.

Peak memory (maximum resident set, `/usr/bin/time -f %M`, `make`
builds). Most benchmarks are a few MB of interpreter and keep little
data. `collections` keeps 4 million ints in a list, 2 million floats, a
map of 500,000 ints and 300,000 objects of four number fields:

| Program | 16 bytes | 8 bytes | |
|---|---|---|---|
| cart | 3.2 MB | 3.1 MB | -4% |
| collatz | 2.8 MB | 2.7 MB | -5% |
| dispatch | 2.9 MB | 2.9 MB | -0% |
| fib | 2.9 MB | 2.8 MB | -2% |
| jsondoc | 73.8 MB | 65.1 MB | -12% |
| loop | 3.1 MB | 2.7 MB | -13% |
| mandelbrot | 2.9 MB | 2.8 MB | -3% |
| strings | 2.8 MB | 2.8 MB | +2% |
| textbuild | 28.5 MB | 23.7 MB | -17% |
| words | 170.0 MB | 154.5 MB | -9% |
| collections | 162.1 MB | 97.1 MB | -40% |

Memory is the clear gain: 9–17% less for the benchmarks that keep data
(`words`, `jsondoc`, `textbuild`), 40% less for collections of numbers.
List items, map entries, fields, the stack and globals all halve.

### Decision

The 16-byte layout stays the default. Under `make`, the build CI
compares, the 8-byte layout is up to 15% slower (5% on the geometric
mean). Under `make release-pgo`, what users run, it is 5% faster on the
geometric mean, but by margins on the order of the 15% that layout
alone moves a PGO build. The instruction counts, which don't depend on
layout, say the int instructions do more work. #121's rule was that the
faster layout becomes the default, and neither is reliably faster.
NaN boxing stays as a tested build option (`make NANBOX=1`) for builds
where memory matters more than the last few percent: 40% less for
collections of numbers.

What would change the answer: the stack top in a register or a register
VM (#123), so that the store-forwarding effects stop deciding `loop` and
`collatz`; or the compiler telling the VM that an int can't leave the
inline range (a loop counter below a list's length), so its instructions
could skip the checks. Measuring the WebAssembly build both ways, which
these numbers don't cover, is left for the playground.

What was tried:

| Change | Result | Kept |
|---|---|---|
| Accessors for every read and write of a value (`plum_as_int`, `plum_set_int`, `plum_is_ref`, ...), the 16-byte layout | within ±2% of main on every benchmark with `make` | yes |
| NaN boxing, the int slow paths inline in each instruction | run loop 32 KB → 50 KB; `mandelbrot` +15% cycles with 11% fewer instructions, front-end bound | no |
| The slow paths (boxed operand or result, overflow) in out-of-line functions | run loop 39 KB; the counts and times above | yes |
| `+` and `-` of two inline ints without the 64-bit overflow check (50-bit operands can't overflow) | `collatz` −1% instructions | yes |
| Range and tag checks with shifts instead of 64-bit constants (`(x << 14) >> 14 == x`) | +8% instructions on `collatz`, +11% on `loop` | no |
| Compare-and-jump on the stored bits of two inline ints (their encoding keeps them in order), without decoding | the same instructions: GCC already did it | no |
| Ints as their low 50 bits, sign-extended to decode (two shifts), instead of offset by a constant | not built: the shift experiment above already costs more than adding a constant | no |
| JavaScriptCore-style layout (pointers as they are, floats offset by 2^49) | not built: every float load and store pays an add, the opposite of where the 8-byte layout gains | no |
| A fixed alignment for the run loop (`-falign-functions`), to take the 15% layout noise out of release builds | done after #121, for both layouts: "Code placement" below | — |

## Code placement

#121 found that two builds with the same run loop could differ by 15% in
cycles: the loop (`execute()` in src/vm/vm.c) had moved by 32 bytes
because code placed before it had changed. Since then the run loop starts
on a 64-byte boundary (`__attribute__((aligned(64)))` with GCC and Clang;
MSVC has no alignment for one function, so Windows builds are as before;
`-DPLUM_RUN_LOOP_ALIGN=0` turns it off).

**Method.** A build of each variant moves the run loop and nothing else:
`vm.c` is compiled with `-fno-toplevel-reorder`, so functions come out in
source order, and with `__asm__(".text\n.skip N, 0xcc")` just before
`execute()`, N = 0, 16, 32 and 48 bytes (GCC aligns functions to 16, so
those are its four possible starts modulo 64). The rest of the program is
the same object files. With the alignment, the four builds put the loop at
the same offset modulo 64. Each variant was then timed with
`perf stat -e cpu_core/cycles/,cpu_core/instructions/`, pinned to one
P-core (`taskset`), the variants taking turns (the order reversed every
round), 11 rounds for `make` and PGO and 7 for LTO; the figure for a
variant is its median. Linux x86_64, Intel Core i5-13450HX, GCC 15.2 and
13.4; the machine was also running other work, which moves cycles by up
to about 1% between rounds. Instructions retired differ by under 0.2%
between variants of a build (PGO: 1%, as the profile differs).

Spread of the median cycles over the four placements (largest / smallest
- 1), main and with the run loop aligned:

| Benchmark | `make`, GCC 15: main | aligned | `make`, GCC 13: main | aligned | `make NANBOX=1`: main | aligned | `release-lto`: main | aligned | `release-pgo`: main | aligned |
|---|---|---|---|---|---|---|---|---|---|---|
| cart | 10.2% | 1.3% | 10.8% | 1.4% | 1.7% | 1.0% | 6.6% | 0.6% | 3.2% | 3.3% |
| collatz | 4.1% | 1.3% | 12.9% | 0.6% | 4.7% | 0.5% | 7.5% | 0.5% | 6.4% | 0.7% |
| dispatch | 5.9% | 0.5% | 8.8% | 0.4% | 0.3% | 0.8% | 3.8% | 0.2% | 0.8% | 1.7% |
| fib | 9.9% | 1.0% | 18.8% | 1.0% | 10.3% | 0.6% | 8.6% | 0.3% | 13.5% | 4.4% |
| jsondoc | 6.0% | 2.9% | 3.4% | 1.9% | 5.8% | 1.3% | 1.4% | 0.7% | 3.9% | 7.3% |
| loop | 0.2% | 0.3% | 15.8% | 3.7% | 0.3% | 0.1% | 0.0% | 0.0% | 0.4% | 0.7% |
| mandelbrot | 14.4% | 0.9% | 20.7% | 0.4% | 20.7% | 1.9% | 16.5% | 0.3% | 23.1% | 1.0% |
| strings | 2.1% | 0.5% | 1.4% | 0.7% | 1.9% | 0.6% | 2.1% | 0.7% | 5.8% | 2.9% |
| textbuild | 2.3% | 3.0% | 4.2% | 2.7% | 5.5% | 2.0% | 2.8% | 1.3% | 2.3% | 2.7% |
| words | 5.1% | 0.9% | 3.0% | 1.2% | 3.7% | 1.6% | 2.7% | 0.9% | 1.7% | 2.3% |

Where the run loop starts was worth up to 23% (`mandelbrot` with PGO:
585 M to 720 M cycles for 3.03 G instructions); aligned, no benchmark
moves by more than 3.7% with `make` and LTO. PGO builds keep some spread
(`jsondoc` 7%, `fib` 4%): each of them is trained and laid out anew, so
more than the run loop's start differs between them.

What moves is mostly the dispatch: the few instructions at the top of the
loop that load the next opcode and jump through the table. In the slow
GCC 15 placement of `mandelbrot` they cross a 64-byte line (`lea` at
offset 52, the indirect `jmp` at 9 of the next line); in the fast ones
they sit within one. The front-end counters (`idq.dsb_uops`,
`idq.mite_uops`, `frontend_retired.dsb_miss`) and branch misses hardly
differ between the two, so the cost is in the back end; we did not find
which structure is responsible. Which offset is fast also differs by
compiler: GCC 13's fast one is GCC 15's slow one.

Alignment pins the loop to one offset; it doesn't choose a good one. In
these harness builds the pinned offset was the slow one for GCC 15
(`mandelbrot` 749–756 M against 654–748 M) and the fast one for GCC 13
(726–729 M against 725–876 M). Built normally, without the harness, the
aligned run loop is level with main in every configuration (median
cycles, 7 rounds each, main and aligned taking turns):

| Benchmark | `make` GCC 15: main | aligned | | `make` GCC 13: main | aligned | | `release-lto`: main | aligned | | `release-pgo`: main | aligned | |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cart | 542 M | 548 M | +1.1% | 578 M | 563 M | -2.6% | 627 M | 627 M | -0.0% | 537 M | 523 M | -2.5% |
| collatz | 1927 M | 1923 M | -0.2% | 1954 M | 1979 M | +1.3% | 2030 M | 2018 M | -0.6% | 1988 M | 1921 M | -3.4% |
| dispatch | 394 M | 395 M | +0.2% | 401 M | 401 M | -0.1% | 460 M | 459 M | -0.2% | 367 M | 369 M | +0.5% |
| fib | 88 M | 88 M | +0.5% | 105 M | 98 M | -6.8% | 117 M | 116 M | -0.6% | 87 M | 88 M | +2.0% |
| jsondoc | 1092 M | 1094 M | +0.1% | 1119 M | 1119 M | +0.0% | 860 M | 876 M | +1.8% | 860 M | 838 M | -2.7% |
| loop | 907 M | 907 M | -0.0% | 394 M | 384 M | -2.6% | 905 M | 905 M | +0.0% | 743 M | 741 M | -0.2% |
| mandelbrot | 641 M | 639 M | -0.3% | 704 M | 722 M | +2.6% | 746 M | 746 M | +0.0% | 722 M | 737 M | +2.1% |
| strings | 672 M | 665 M | -1.0% | 696 M | 701 M | +0.7% | 586 M | 581 M | -0.8% | 548 M | 537 M | -1.9% |
| textbuild | 209 M | 216 M | +3.5% | 215 M | 215 M | +0.3% | 192 M | 189 M | -1.4% | 173 M | 171 M | -1.5% |
| words | 1217 M | 1209 M | -0.7% | 1257 M | 1251 M | -0.5% | 1141 M | 1132 M | -0.8% | 1046 M | 1032 M | -1.4% |

Geometric mean, aligned against main: 1.003 (`make`, GCC 15), 0.992
(GCC 13), 0.997 (LTO), 0.991 (PGO). A future change to the run loop
itself still moves its code, and may land on a slow offset; what the
alignment removes is the noise from changes anywhere else, so a pull
request's numbers measure the pull request. (`loop` runs in about 390 M
cycles with GCC 13 and 907 M with GCC 15 in every placement: that is a
difference in the code the two compilers generate, not in where it
lands: GCC 15 made the compare-and-jump a conditional move, which #123
found and fixed; "Superinstructions (#123)" below.)

What was tried:

| Change | Result | Kept |
|---|---|---|
| `aligned(64)` on `execute()` | the tables above: spread from up to 23% to under 4% (PGO 7%), level with main | yes |
| `-falign-loops=64` for the whole build | pins the dispatch itself, wherever the function starts: the jump sat at the same offset in all four placements, and spreads fell to 0.3–2.6% with GCC 13, 0.0–1.4% with `NANBOX=1` and 0.1–3.8% with GCC 15. But the offset it pins is the slow one for GCC 13: against main, `mandelbrot` +27%, `loop` +19%, `collatz` +18%, `dispatch` +12%, `cart` +11%; and the binary grows by 27 KB (2.6%) | no |
| `-falign-jumps=32` for vm.c, with and without `aligned(64)` | doesn't touch the dispatch (it is also reached by falling through, which `-falign-jumps` leaves alone): GCC 15 `mandelbrot` still spread 15% without the function alignment | no |
| Keeping `vm->top` in a local (a register) in the run loop | not tried for this: it would change the code that is measured, not where it lands, and every instruction that calls out of the loop would have to store it back (123 uses). #120's attempt at keeping the frame's slot base in a local helped GCC 15 by 2–3% and slowed GCC 13 by 19–50%; the register VM of #123 is where this belongs | — |

The harness builds aren't the normal builds: `-fno-toplevel-reorder`
changes where GCC puts `vm.c`'s functions, which is why the normal and
harness builds can land on different offsets. Clang has `aligned` on
functions too (the macOS release builds use it); MSVC has no equivalent,
and its builds are unchanged.

## Reference counts (#122)

Every copy of a heap value counts a reference and every drop releases
one. #122 asked to skip the counting that is provably unnecessary, with
`strings` and a collection-heavy benchmark at least 1.5 times as fast.
`lists` is that benchmark (committed, with its Python, Lua and Node.js
twins, before any of the changes): lists and maps handed to functions
that read them, a list indexed in a loop, a grid read by column, words
grouped in a `map[str, list[str]]`.

**Profile first** (`perf record`, `make` with `-g`, main):

- `lists`: 77% in the run loop, 10% in `plum_release`, 4.5% in
  `call_builtin` (`len`). Counting the dynamic instructions (a
  throw-away counter in the run loop) showed what pushes references:
  24.4 M of its 165 M instructions pushed an object, 19.6 M of them a
  local list just to index it (`GET_LOCAL xs`, `GET_LOCAL i`,
  `INDEX_LIST`: a count up, then a call to `plum_release` to count it
  down) and 4 M a local string for `len()`.
- `strings`: 8% in `plum_release`, but about 25% in `snprintf` and its
  helpers, 10% in `malloc` and `free` and 10% in `plum_value_text_with`.
  Each `{i}` made a temporary string (`TO_STRING`: a growable buffer,
  `snprintf`, a count of its characters, a copy into a new string) that
  `BUILD_STRING` copied and freed at once.

So for `lists` the counting is the cost, and for `strings` it is the
temporaries: counted, but mostly allocated and formatted.

**What changed:**

| Step | What | `lists` cycles | `strings` cycles | instructions |
|---|---|---|---|---|
| main | | 1304 M | 676 M | 6.17 G / 2.69 G |
| borrowed locals | `xs[i]`, `len(s)` read the list or string in its slot: `INDEX_LIST_L`, `LEN_L`, nothing counted | 964 M | 653 M | 4.46 G / 2.64 G |
| no temporaries in text | an int, bool or str part of an interpolation goes into the result as it is (`BUILD_TEXT`); `BUILD_STRING` drops its parts inline | 991 M | 393 M | 4.45 G / 1.56 G |
| moves | a local moves on its last read (`MOVE_LOCAL`), `RETURN_LOCAL` moves, `xs[i]` with both in locals is one instruction (`INDEX_LIST_LL`) | 882 M | 402 M | 4.00 G / 1.56 G |
| inline drops | an index, `in` or string comparison drops its operands inline while something else holds them | 865 M | 393 M | 3.94 G / 1.57 G |

(`make`, GCC 15, medians of 7 interleaved rounds of `perf stat` pinned to
one core, measured before rebasing onto the run loop alignment; the
"no temporaries" step's `lists` cycles moved up with the same
instructions, which is code placement.) `jsondoc` gained 8% from the text
step (its keys are interpolated) and `words` 3% from the drops; nothing
else changed by more than placement noise.

**Results**, against main with the run loop aligned (#186), medians of
11 interleaved rounds pinned to one P-core. Cycles and instructions from
`perf stat`; times from `bench/run.py --baseline --pairs 11 --plum-only`:

| Benchmark | `make`: cycles main | #122 | | time main | #122 | | PGO: cycles main | #122 | | time main | #122 | | `NANBOX=1`: cycles main | #122 | |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cart | 544 M | 547 M | 0.99x | 0.123 s | 0.124 s | 0.99x | 528 M | 532 M | 0.99x | 0.114 s | 0.116 s | 0.99x | 589 M | 586 M | 1.01x |
| collatz | 1940 M | 1936 M | 1.00x | 0.446 s | 0.444 s | 1.00x | 1923 M | 1986 M | 0.97x | 0.422 s | 0.434 s | 0.97x | 2232 M | 2169 M | 1.03x |
| dispatch | 399 M | 396 M | 1.01x | 0.088 s | 0.087 s | 1.01x | 371 M | 354 M | 1.05x | 0.080 s | 0.078 s | 1.02x | 377 M | 373 M | 1.01x |
| fib | 90 M | 92 M | 0.98x | 0.019 s | 0.020 s | 0.97x | 87 M | 93 M | 0.94x | 0.019 s | 0.020 s | 0.95x | 97 M | 101 M | 0.96x |
| jsondoc | 1140 M | 1083 M | 1.05x | 0.237 s | 0.215 s | 1.10x | 919 M | 855 M | 1.07x | 0.191 s | 0.177 s | 1.08x | 1070 M | 995 M | 1.08x |
| **lists** | 1315 M | 880 M | **1.49x** | 0.297 s | 0.190 s | **1.56x** | 1239 M | 835 M | **1.48x** | 0.269 s | 0.178 s | **1.51x** | 1263 M | 803 M | **1.57x** |
| loop | 911 M | 910 M | 1.00x | 0.199 s | 0.200 s | 1.00x | 742 M | 743 M | 1.00x | 0.162 s | 0.162 s | 1.00x | 1021 M | 1020 M | 1.00x |
| mandelbrot | 647 M | 655 M | 0.99x | 0.142 s | 0.142 s | 1.00x | 746 M | 710 M | 1.05x | 0.162 s | 0.154 s | 1.05x | 615 M | 622 M | 0.99x |
| **strings** | 673 M | 388 M | **1.73x** | 0.152 s | 0.087 s | **1.75x** | 549 M | 349 M | **1.57x** | 0.117 s | 0.075 s | **1.56x** | 663 M | 394 M | **1.68x** |
| textbuild | 222 M | 217 M | 1.02x | 0.048 s | 0.047 s | 1.04x | 185 M | 199 M | 0.93x | 0.037 s | 0.040 s | 0.93x | 200 M | 199 M | 1.00x |
| words | 1279 M | 1266 M | 1.01x | 0.264 s | 0.260 s | 1.02x | 1116 M | 1105 M | 1.01x | 0.233 s | 0.233 s | 1.00x | 1197 M | 1187 M | 1.01x |

Instructions: `lists` 6.17 G → 3.92 G, `strings` 2.68 G → 1.56 G,
`jsondoc` 3.99 G → 3.76 G with `make`; the rest within 0.5% in all
three builds, except PGO's `textbuild` (+2.7%): `lists` is now part of
the PGO training (tools/pgo-train.sh runs every benchmark), which shifts
the profile, and `textbuild` loses 7% there. `fib`'s 2–6% in cycles comes
with the same instructions: placement of code outside the run loop.

**Verdict.** `strings` is 1.56–1.75 times as fast in every build.
`lists` reaches 1.5 by time (1.51–1.56x) and in the NaN-boxed build
(1.57x), but in cycles with the 16-byte layout it sits just under it
(1.48–1.49x). What `lists` still spends is no longer counting: after
these changes under 1 M of its 141 M instructions push a reference. It is
the dispatch and the stack traffic of int arithmetic (`s += a[i] * b[i]`
is still `GET_LOCAL`, two `INDEX_LIST_LL`, `MUL_INT`, `ADD_INT`,
`SET_LOCAL`), which fewer reference counts can't remove; a register VM
(#123) can. Against the other interpreters on the same machine, `lists`
takes 0.179 s, CPython 3.14 0.482 s (2.70x), Lua 5.3 0.307 s (1.72x),
Node.js 0.101 s; `strings` 0.084 s, CPython 0.164 s (1.94x), Lua 0.127 s
(1.50x).

**Value semantics.** A move empties a slot that no path reads again
before writing it, and the analysis is conservative: every instruction
that reads a local is listed in `compiler/moves.c` (and
`tests/bytecode-documented.sh` fails if an instruction whose
description names a local isn't), a closure's variables live in cells
and are never moved, `MAKE_CLOSURE` counts as reading every slot, and
inside a `try` the handler counts as following every instruction.
`tests/lang/memory/moves.plum` checks what a wrong move or borrow would
break: a list passed then read, copied then changed, read again next
time round a loop, captured by a closure, put in a field, a list or a
map, sent to a task or over a channel, changed by a method, read in a
`catch` or `finally` (nested too), index errors with locals, and the
order of `to_str()` calls in an interpolation; it prints the same with
main's binary. The whole suite passes in both layouts, and
`--check-leaks` and `--mem-stats` report the same as main's for every
test, example and benchmark (141 programs; memory at exit is 0 for all
of them), apart from the counts of strings and lists made: fewer, since
the temporaries and the copies a move avoids are no longer made
(`moves.plum`: 37 lists instead of 43).

**What was tried, or not:**

| Change | Result | Kept |
|---|---|---|
| Moving any local on its last read | an object can then be freed before its variable goes out of scope, which a program can see: its weak references read none (`weak_after_last_use` in the test), a file closes. Moves are limited to slots that only ever hold numbers, strings and collections of them | limited |
| Moving a module variable | not done: a function called in between can read it | no |
| `MOVE_LOCAL` for slots of numbers | nothing to save (no reference), so a number slot keeps `GET_LOCAL` | no |
| `BUILD_TEXT` inline in the run loop | 2–4% more instructions on `collatz`, `fib` and `mandelbrot` in the NaN-boxed build, which never run it; out of line, level | no |
| Parts with a format, floats and objects in `BUILD_TEXT` | not done: a format can fail and `to_str()` runs code, which must happen in order (the test checks it); floats need their own buffer per part. They keep `TO_STRING` | no |
| Borrowed parameters (the callee doesn't own its arguments) | not done: every return, unwind and tail call would have to know which slots it may not release, and a move out of such a slot would give away a reference it doesn't hold. The profile shows a call's arguments cost one count each per call, not per iteration | no |
| Borrowing an indexed temporary (`grid[r][c]`'s row) | not done: the row's count goes up and down inline now; the rest is a new instruction per shape | no |
| Fewer instructions for int arithmetic on list items | out of #122's scope (it is not counting); #123 did it (`LOCAL_ADD_INT`) | — |

`--mem-stats` reports fewer strings made by programs that interpolate
ints and bools (`tests/lang/memory/mem_stats.plum`: 17 instead of 27),
since those temporaries are no longer made.


## Superinstructions (#123)

#123 asked which pairs of instructions to join, and whether a
register-based VM would cut dispatch further. Linux x86_64, Intel Core
i5-13450HX, GCC 15.2 unless marked (GCC 13.4 too, Clang isn't installed on
this machine). Cycles, instructions and times are medians of interleaved
rounds (the builds taking turns, the order reversed every round) of
`perf stat -e cpu_core/cycles/,cpu_core/instructions/,task-clock`,
pinned to one P-core (`taskset -c 6`), 7 rounds while choosing and 11 for
the final tables. The machine was running other work, which moved single
benchmarks by up to 5% between runs with the same instructions; every
number that decided something was measured again.

**Measure first.** A build with `-DPLUM_OP_COUNTS=1` counts every
instruction it runs, every pair of instructions run one after the other
(apart, where the second was jumped to) and every triple, and adds them to
the file named by `PLUM_OP_COUNTS` at exit; normal builds have none of it.
`bench/oppairs.py` runs bench/ and examples/ with it and prints the
tables:

```bash
make DEFS=-DPLUM_OP_COUNTS=1 BUILD=build/opcounts
python3 bench/oppairs.py build/opcounts/plum --top 15 --triples
```

The top 15 pairs on main, where the second instruction is the next one in
the code (a superinstruction could join them), over the 16 programs (780 M
instructions run). "Of all" weights each pair by how often it ran; "mean
share" is the mean of its share in each program, so `collatz` and `loop`
don't decide alone:

| Pair (in code order) | run | of all | mean share | most in |
|---|---|---|---|---|
| `MOD_INT_LK` `JUMP_IF_NOT_EQ_INT_K` | 55.7 M | 7.1% | 2.7% | loop 30%, collatz 13% |
| `LOCAL_ADD_INT_K` `LOOP` | 41.4 M | 5.3% | 1.1% | collatz 13%, mandelbrot 4% |
| `JUMP_IF_NOT_NE_INT_LK` `MOD_INT_LK` | 35.7 M | 4.6% | 0.8% | collatz 13% |
| `LOCAL_FLOORDIV_INT_K` `JUMP` | 23.8 M | 3.1% | 0.5% | collatz 9% |
| `JUMP_IF_NOT_EQ_INT_K` `LOCAL_FLOORDIV_INT_K` | 23.8 M | 3.1% | 0.5% | collatz 9% |
| `GET_LOCAL` `MUL_FLOAT_L` | 23.2 M | 3.0% | 1.1% | mandelbrot 17% |
| `SET_LOCAL` `LOCAL_ADD_INT_K` | 17.6 M | 2.3% | 0.5% | collatz 4%, mandelbrot 4% |
| `ADD_INT_K` `SET_LOCAL` | 11.8 M | 1.5% | 0.3% | collatz 4% |
| `CONST` `MUL_INT_L` | 11.8 M | 1.5% | 0.3% | collatz 4% |
| `MUL_INT_L` `ADD_INT_K` | 11.8 M | 1.5% | 0.3% | collatz 4% |
| `GET_LOCAL` `INDEX_LIST_LL` | 11.6 M | 1.5% | 0.6% | lists 10% |
| `SET_LOCAL` `FOR_NEXT` | 11.6 M | 1.5% | 0.6% | lists 10% |
| `ADD_INT` `SET_LOCAL` | 11.6 M | 1.5% | 0.6% | lists 10% |
| `MUL_FLOAT_L` `GET_LOCAL` | 11.6 M | 1.5% | 0.5% | mandelbrot 9% |
| `ADD_FLOAT_L` `SET_LOCAL` | 11.5 M | 1.5% | 0.5% | mandelbrot 9% |

The most-run pairs with a jump between them: `LOOP` then the `while`
test (`JUMP_IF_NOT_NE_INT_LK`, 4.6%), `JUMP` then `LOCAL_ADD_INT_K`
(3.1%), `FOR_NEXT` then `MOD_INT_LK` (2.6%), `LOOP` then `FOR_EACH`
(2.3%, the highest mean share of any pair: 2.4%). The single
instructions: `MOD_INT_LK` 7.8%, `LOOP` 7.6%, `GET_LOCAL` 7.5%,
`JUMP_IF_NOT_EQ_INT_K` 7.1%, `SET_LOCAL` 5.8%; by mean share `CONST`
(9.3%) and `GET_LOCAL` (7.9%) lead. The examples run 4,500 instructions in all and
change none of this.

**First, a finding that wasn't a superinstruction.** `loop` ran in 904 M
cycles with GCC 15 and 387 M with GCC 13, with the same instructions to
within 3% ("Code placement" and "Value layout" above put it down to the
stack top's store and reload). `perf annotate` shows why: GCC 15 compiled
`if (!holds) ip += d`, the end of every compare-and-jump, into a
conditional move. The address of the next instruction then waits for the
comparison, and in `loop` the comparison waits for a division
(`MOD_INT_LK`), so nothing after it can start: 45 cycles a time round.
GCC 13 kept a branch, which the processor predicts and runs past. An
empty `asm volatile("")` in the taken path (`TAKE_JUMP` in src/vm/vm.c,
GCC and Clang, not WebAssembly) keeps it a branch: `loop` −57%, `collatz`
−18%, `mandelbrot` −4% with GCC 15, the rest within ±3%; GCC 13's code
already branched and is unchanged (geometric mean 1.000). The NaN-boxed
build had the same conditional move (its `loop` at 1014 M cycles, likely
the "1011 M" of "Value layout"); `make release-pgo` had none, so what
kept PGO's `loop` at 739 M is something else.

**Superinstructions.** Each was built on its own (the compiler emitting
it or not, the VM the same) and measured against the build without it,
GCC 15, 7 rounds:

| Instructions | Instead of | Where | Cycles | Instructions run | Kept |
|---|---|---|---|---|---|
| `JUMP_IF_NOT_MOD_EQ0_LK`, `_NE0_LK`: `if x % k == 0` (or `!= 0`) for an int local and a literal from 1 to 32767 | `MOD_INT_LK`, `JUMP_IF_NOT_EQ_INT_K` (a push, a pop and a constant load) | `loop`, `collatz` | loop −10.7%, collatz −9.7% | loop −32%, collatz −15% | yes |
| `LOCAL_ADD_INT`, `LOCAL_SUB_INT`: `s += v` and `s = s + v` on an int local, `v` on the stack | `GET_LOCAL s`, ..., `ADD_INT`, `SET_LOCAL s` | `lists` (`s += a[i] * b[i]`, `s += grid[r][c]`) | lists −18.3% | lists −14% | yes |
| `ADD_INT_LL` ... `MUL_FLOAT_LL`: local op local, two ints or two floats, `+ - *` | `GET_LOCAL`, `MUL_FLOAT_L` | `mandelbrot` (`x * x`, `y * y`) | mandelbrot −15.9% | mandelbrot −13% | yes |
| `LOOP_IF_LT_INT_LK` ...: a `while` on an int local against a literal tests again at the bottom, counting a step as `LOOP` did | `LOOP`, `JUMP_IF_NOT_NE_INT_LK` | `collatz` (`while n != 1`) | collatz −12.9% | collatz −5% | yes |
| `LOOP_FOR_EACH`: `LOOP` back to a `FOR_EACH` running it with no dispatch of its own | `LOOP`, `FOR_EACH` | `lists`, `cart`, `dispatch`, `words` | lists −1.8%, cart +1.3%, the rest ±0.3% (11 rounds) | lists −1.5%, cart −0.8% | no: within noise; a dispatch that is always predicted costs little next to `FOR_EACH`'s own work |
| `FOR_EACH_NEXT`: `for x in xs` tested at the bottom, like `FOR_NEXT` (one `FOR_EACH` case taking the place of the code to go on or out) | `LOOP`, `FOR_EACH` | the same | — | +8.6% on `mandelbrot`, which runs no `FOR_EACH` | no: GCC allocated the whole run loop's registers worse |

Each gives the same results, the same errors at the same lines and the
same step counts as what it replaces. Every other benchmark moved by under
±3%, with the same instructions. The tests: `tests/lang/arith/fused.plum`
(every new instruction: negative, power-of-two, 32-bit and 64-bit
operands, ints at the edges, NaN, infinities and -0.0, every loop
comparison with `continue` and `break`, the overflow errors and their
messages), `tests/lang/errors/fused_add_overflow.plum` and
`fused_mul_overflow.plum` (an uncaught overflow: its line and stack trace),
and a unit test that a step-limited `while` stops after the same output as
main's. All of them print the same with main's binary, in both value
layouts. So did 600 random programs (int and float arithmetic on locals
near the overflow edges, `+=`, `x % k` conditions, `while` loops with
`break` and `continue`, uncaught errors in half of them): stdout, stderr
and exit status were byte-identical to main's, `make` and `NANBOX=1`.

**The stack top in a register.** The run loop split in two: an outer
`switch` for the instructions of hot loops (constants, locals, int and
float arithmetic, the compare-and-jumps, `LOOP`, `FOR_*`, fields of a
local, `INDEX_LIST_LL`), which keep the top in a local `top`, and an inner
`switch` for every other one, with `vm->top` stored before it and loaded
after; errors store it before they unwind. Measured on top of the branch
fix, against it, 5 rounds:

| Benchmark | GCC 15: cycles | instructions | GCC 13: cycles | instructions |
|---|---|---|---|---|
| cart | +19.2% | +8.0% | +8.5% | +5.6% |
| collatz | +19.1% | +7.4% | −3.9% | +4.2% |
| dispatch | +6.1% | +7.2% | +4.3% | +5.4% |
| fib | +31.8% | +9.5% | +3.1% | +6.8% |
| jsondoc | +0.3% | +0.3% | −0.9% | +0.4% |
| lists | +1.8% | +5.5% | +2.4% | +4.1% |
| loop | +3.6% | +5.2% | +8.1% | +4.7% |
| mandelbrot | +18.7% | +4.9% | −3.1% | +1.5% |
| strings | +6.9% | +1.0% | −1.0% | +0.8% |
| textbuild | +0.8% | +2.6% | +2.0% | +3.7% |
| words | +2.8% | +2.9% | +1.7% | +3.6% |

Geometric mean: +9.7% cycles with GCC 15, +1.9% with GCC 13. It runs
more instructions in both: the second dispatch for every instruction of
the inner switch (calls among them, so `fib`), and with one more variable
live across the whole loop GCC spills more, so even `loop`, which runs only
outer-switch instructions, runs 5% more. The store-and-reload of
`vm->top` wasn't what bound the short loops (the conditional move above
was), so there was little to win. Not kept; Clang wasn't measured (not on
this machine). The slot base was tried in #120 with the same outcome.

**A register VM.** The prototype: 3-address instructions on a frame's
slots, `local [a] = local [b] op local [c]` and with a literal on either
side (`R_ADD_INT`, `R_MUL_INT_KL`, ..., `R_MUL_FLOAT`, `R_ADD_FLOAT_K`,
23 instructions with `R_MOVE` and two float compare-and-jumps on a slot),
and a compiler rule that computes `x = e` and `plant x = e` straight into
`x`'s slot when `e` is `+ - *` (and `//`, `%` by a literal) over int or
float locals and literals, with hidden slots for the intermediate values,
and a float comparison against a literal into a hidden slot. Errors keep
their operands in source order (a literal on the left has its own
instructions), so messages, lines and the 600 random programs stayed
byte-identical to main's. `collatz`'s `n = 3 * n + 1` becomes two
instructions instead of four; `mandelbrot`'s loop body four, three and one
instead of five, five and two. The same binary was measured with the rule
on and off (an environment variable), on top of the superinstructions:

| Benchmark | VM instructions run | cycles, GCC 15 | instructions | cycles, GCC 13 |
|---|---|---|---|---|
| collatz | 204.7 M → 180.7 M (−11.7%) | −10.5% | −9.5% | −4.6% |
| mandelbrot | 111.7 M → 88.8 M (−20.5%) | −19.6% | −8.8% | −22.0% |
| the other nine | unchanged | −1.2% to +1.1% | ±0.1% | lists, loop −0.5%, fib −2.7% (the rest not measured) |

Each instruction the register form saved was worth 4 to 6 cycles. To
estimate a full rewrite, count what it could remove: the instructions
that only move values between the stack and the slots, globals or
constants (`GET_LOCAL`, `SET_LOCAL`, `CONST`, `MOVE_LOCAL`, `POP`,
`GET_GLOBAL`, `SET_GLOBAL`, ...), after the superinstructions:

| Benchmark | instructions run | of them moves | cycles per instruction | estimated saving |
|---|---|---|---|---|
| cart | 72.3 M | 22% | 7.6 | 4–17% |
| collatz | 204.7 M | 12% | 6.4 | 3–12% (prototype: 10.5%) |
| dispatch | 50.0 M | 28% | 7.7 | 5–22% |
| fib | 10.8 M | 0% | 8.0 | 0% |
| jsondoc | 3.7 M | 47% | 261 | 0–1% |
| lists | 98.5 M | 6% | 6.9 | 1–6% |
| loop | 46.7 M | 0% | 6.8 | 0% |
| mandelbrot | 111.7 M | 27% | 4.5 | 9–35% (prototype: 19.6%) |
| strings | 7.5 M | 53% | 52 | 2–6% |
| textbuild | 2.5 M | 45% | 83 | 1–3% |
| words | 34.1 M | 41% | 33 | 2–7% |

(The range: half of the moves gone at 3 cycles each, to all of them at 6.)
On the geometric mean that is 3% to 11%. The benchmarks that spend their
time in the standard library, allocation or reference counts (`jsondoc`,
`strings`, `textbuild`, `words`: 33 to 261 cycles per instruction) gain
almost nothing, and `fib` and `loop` already run no moves. Most of what is
left in `cart` and `dispatch` is a call's arguments, which a register VM
still has to copy into the callee's frame (Lua's `MOVE` before `CALL`), so
the upper end there is unlikely.

The cost of a full rewrite: every expression path of the compiler
(src/compiler/compiler.c, about 4,000 lines) emits stack code and would need a
register allocator for temporaries; every instruction is re-encoded with a
destination (wider instructions, more bytecode); calls, natives
(`call_value` and the standard library take their arguments on the
stack), tasks (each has its own stack), `try` (unwinds to a stack height)
and the disassembler change with it; the opcode byte has 48 values left
(208 used), so a register family beside the stack one doesn't fit without
a new encoding. The isolates constraint (#150) is no obstacle: the
bytecode stays read-only either way.

**Recommendation for 3.0.** Not a rewrite. The measured gain is in
arithmetic on locals (`mandelbrot` −20%, `collatz` −5 to −11%), which a
hybrid gets: register-form instructions for arithmetic expressions over
locals, as in the prototype, beside the stack for calls and everything
else. 3.0 can change the bytecode format, which makes room for them (a
second opcode page, or 3-address forms that replace the `_K`, `_L`, `_LK`
and `_LL` families instead of adding to them). The prototype itself isn't
merged: 23 instructions for two benchmarks would use half the opcode space
left, which is the decision for 3.0's encoding to make. What would change
the answer: programs whose time is in moves between the stack and the
slots, which in this suite only `mandelbrot` is.

**Results.** Main against this change (the branch fix and the four kept
superinstructions), 11 interleaved rounds. `make` with GCC 15, with the
branch fix alone in the middle:

| Benchmark | cycles: main | branch fix | #123 | | instructions: main | #123 | | time: main | #123 | |
|---|---|---|---|---|---|---|---|---|---|---|
| cart | 556 M | 557 M | 583 M | +4.9% | 2.65 G | 2.65 G | −0.1% | 121 ms | 127 ms | +4.9% |
| collatz | 2025 M | 1652 M | 1356 M | −33.0% | 6.02 G | 4.81 G | −20.0% | 442 ms | 296 ms | −33.0% |
| dispatch | 390 M | 402 M | 386 M | −1.0% | 1.92 G | 1.91 G | −0.2% | 85 ms | 84 ms | −1.0% |
| fib | 88 M | 88 M | 86 M | −2.5% | 0.42 G | 0.42 G | −0.6% | 19 ms | 19 ms | −2.6% |
| jsondoc | 984 M | 960 M | 1033 M | +4.9% | 3.76 G | 3.76 G | −0.1% | 215 ms | 225 ms | +4.9% |
| lists | 822 M | 811 M | 688 M | −16.4% | 3.92 G | 3.36 G | −14.2% | 179 ms | 150 ms | −16.4% |
| loop | 904 M | 392 M | 315 M | −65.1% | 1.94 G | 1.31 G | −32.4% | 197 ms | 69 ms | −65.1% |
| mandelbrot | 642 M | 615 M | 505 M | −21.3% | 3.02 G | 2.61 G | −13.6% | 140 ms | 110 ms | −21.3% |
| strings | 388 M | 389 M | 385 M | −0.8% | 1.56 G | 1.56 G | +0.0% | 85 ms | 84 ms | −0.8% |
| textbuild | 203 M | 205 M | 203 M | −0.1% | 0.72 G | 0.72 G | +0.0% | 44 ms | 44 ms | +0.0% |
| words | 1141 M | 1150 M | 1144 M | +0.3% | 3.93 G | 3.93 G | −0.1% | 249 ms | 250 ms | +0.3% |

Geometric mean of cycles 0.848 (the branch fix alone 0.907). `cart` and
`jsondoc` run none of the new instructions and the same instructions as
main; measured again, 11 rounds each, they were level (cart 537 M and
537 M, jsondoc 939 M and 939 M).

`make` with GCC 13 (what CI builds with), `make release-pgo` and `make
NANBOX=1` (GCC 15; 7 rounds), cycles:

| Benchmark | GCC 13: main | #123 | | PGO: main | #123 | | NaN boxing: main | #123 | |
|---|---|---|---|---|---|---|---|---|---|
| cart | 585 M | 579 M | −1.0% | 528 M | 508 M | −3.8% | 582 M | 586 M | +0.6% |
| collatz | 2048 M | 1576 M | −23.0% | 1983 M | 1388 M | −30.0% | 2226 M | 1581 M | −29.0% |
| dispatch | 415 M | 402 M | −3.1% | 349 M | 348 M | −0.3% | 366 M | 365 M | −0.1% |
| fib | 97 M | 95 M | −2.3% | 89 M | 83 M | −6.7% | 98 M | 84 M | −13.8% |
| jsondoc | 999 M | 974 M | −2.5% | 787 M | 774 M | −1.7% | 961 M | 913 M | −4.9% |
| lists | 845 M | 740 M | −12.4% | 798 M | 664 M | −16.9% | 777 M | 733 M | −5.6% |
| loop | 387 M | 284 M | −26.5% | 739 M | 301 M | −59.2% | 1014 M | 307 M | −69.8% |
| mandelbrot | 763 M | 595 M | −22.0% | 703 M | 614 M | −12.7% | 616 M | 512 M | −16.9% |
| strings | 418 M | 436 M | +4.3% | 345 M | 310 M | −10.1% | 389 M | 392 M | +0.8% |
| textbuild | 197 M | 202 M | +2.6% | 183 M | 182 M | −0.6% | 186 M | 185 M | −0.5% |
| words | 1164 M | 1159 M | −0.4% | 1026 M | 1031 M | +0.5% | 1107 M | 1085 M | −1.9% |

Geometric mean: GCC 13 0.915, PGO 0.848, NaN boxing 0.834. Instructions
run fall by the same amounts in every build (`loop` −32 to −34%,
`collatz` −20 to −21%, `lists` −12 to −15%, `mandelbrot` −11 to −15%).
`strings` with GCC 13 is +4.3% with 0.3% fewer instructions, again in a
second run of 11 rounds (+4.2%). It runs no new instruction: the time
went to `build_text` (20% of its cycles on main, 24% here), whose machine
code is byte for byte the same in both binaries and starts at the same
offset in its 64-byte line; only its address differs. We take it for
placement, as in "Code placement".

`bench/run.py --baseline` (main's binary and this one in turns, 11 pairs,
medians, `taskset -c 6`), times:

| Benchmark | `make`: main | #123 | | `release-pgo`: main | #123 | |
|---|---|---|---|---|---|---|
| cart | 0.118 s | 0.117 s | −0.1% | 0.115 s | 0.110 s | −4.6% |
| collatz | 0.442 s | 0.297 s | −33.0% | 0.433 s | 0.303 s | −29.9% |
| dispatch | 0.085 s | 0.085 s | −1.0% | 0.076 s | 0.076 s | +0.1% |
| fib | 0.019 s | 0.019 s | −2.6% | 0.020 s | 0.018 s | −6.9% |
| jsondoc | 0.207 s | 0.207 s | +0.0% | 0.171 s | 0.170 s | −0.3% |
| lists | 0.180 s | 0.149 s | −17.4% | 0.174 s | 0.137 s | −21.3% |
| loop | 0.197 s | 0.069 s | −65.0% | 0.161 s | 0.066 s | −59.1% |
| mandelbrot | 0.140 s | 0.110 s | −21.4% | 0.153 s | 0.134 s | −12.5% |
| strings | 0.084 s | 0.084 s | −0.5% | 0.075 s | 0.068 s | −10.4% |
| textbuild | 0.044 s | 0.044 s | −0.2% | 0.039 s | 0.039 s | −0.4% |
| words | 0.253 s | 0.254 s | +0.6% | 0.230 s | 0.231 s | +0.5% |

With `python3 bench/run.py --runs 7` (fastest of seven, `make`, pinned),
`loop` takes 0.069 s against CPython 3.14's 0.699 s (10.2 times as fast;
3.6 before), `collatz` 0.296 s (7.7 times; Node.js takes 0.245 s),
`mandelbrot` 0.109 s (7.8 times), `lists` 0.151 s (3.2 times).

**What was tried:**

| Change | Result | Kept |
|---|---|---|
| Conditional jumps kept as branches (`TAKE_JUMP`) | GCC 15: loop −57%, collatz −18%, mandelbrot −4%; GCC 13 unchanged | yes |
| `JUMP_IF_NOT_MOD_EQ0_LK` / `_NE0_LK`, `LOCAL_ADD_INT` / `LOCAL_SUB_INT`, the `_LL` forms, `LOOP_IF_*_INT_LK` | the tables above | yes |
| `LOOP_FOR_EACH` (`LOOP` into `FOR_EACH` with one dispatch) | lists −1.8%, cart +1.3%: noise | no |
| `FOR_EACH_NEXT` (a bottom-tested `for x in xs`) | the run loop ran 8.6% more instructions on `mandelbrot`, which doesn't use it | no |
| `x % k == c` for any literal `c` | needs a fourth operand, which the instruction format and `compiler/moves.c` don't have; `== 0` and `!= 0` cover every use in the suite | no |
| The stack top in a local, two-level `switch` | +9.7% cycles with GCC 15, +1.9% with GCC 13, more instructions in both | no |
| Register-form arithmetic on locals (23 instructions) | collatz −10.5%, mandelbrot −19.6% (GCC 15); −4.6%, −22.0% (GCC 13); the rest level | no: an evaluation for 3.0's bytecode (above) |
