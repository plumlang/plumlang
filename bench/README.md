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

The table predates the cheaper calls of #120: `fib`, `cart`, `dispatch`,
`collatz` and `mandelbrot` are faster now (see "Calls (#120)" below).
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
of five, pinned to one core):

| Change | fib(32) cycles | Kept |
|---|---|---|
| before | 579 M | |
| Frames of 24 bytes (no closure pointer: a closure's captures are read through the function value under slot 0), `CALL_GLOBAL` for a declared function called by name (not pushed, no reference counted), methods called without moving their arguments, the rare call checks in one test, an inline return | 433 M | yes |
| `JUMP_IF_NOT_*_INT_LK` (an int local against a literal) and `RETURN_LOCAL` | 292 M | yes |
| Releasing a returning call's slots: a scan without calls first, so a frame of numbers spills no registers | 282 M | yes |
| `CALL_SELF`: a function calling itself reads itself from its frame | 238 M | yes |
| Threaded dispatch (computed goto) for the eight hottest instructions | 233 M; 243 M with `-fno-gcse -fno-crossjumping`, which GCC needs to keep the jumps apart | no |
| A switch over all 256 byte values, to drop its range check | 243 M: GCC keeps the check | no |
| Small int literals as instruction operands instead of constants | 2.5% fewer instructions, the same cycles | no |
| `RETURN_ADD_INT` (`return a + b` of ints), computed without a round trip through the stack | 213 M | yes |

Tail calls (`return f(...)` of a function's own call, `TAIL_SELF`) and
inlining small functions that can't fail don't change these benchmarks;
they let such code run without the recursion limit, or without a call
(spec 6.5). A call now costs about 155 instructions and 30 cycles. What
bounds it is the stack top and the frame living in memory: each
instruction reads and writes `vm->top`, and every argument is stored and
read back once, which keeping the stack top in a register or a register
VM (#123) would remove.

Before and after, Plumlang alone, `make` (`-O2`), best of three
alternating runs of `bench/run.py --runs 5 --plum-only` pinned to one
core (the machine above, in power-saving mode, so slower than the table
at the top):

| Benchmark | before | after | |
|---|---|---|---|
| cart | 0.491 s | 0.391 s | 1.26x |
| collatz | 0.917 s | 0.709 s | 1.29x |
| dispatch | 0.425 s | 0.389 s | 1.09x |
| fib | 0.077 s | 0.029 s | 2.66x |
| jsondoc | 0.341 s | 0.339 s | 1.01x |
| loop | 0.314 s | 0.322 s | 0.98x |
| mandelbrot | 0.252 s | 0.220 s | 1.15x |
| strings | 0.219 s | 0.225 s | 0.97x |
| textbuild | 0.066 s | 0.065 s | 1.02x |
| words | 0.388 s | 0.392 s | 0.99x |

`loop`, `strings` and `words` run none of the changed instructions;
counting cycles, they are 2.9%, 2.0% and 0.9% slower in the `-O2` build,
which is GCC laying out the large run loop differently. In the release
build (`make release-pgo`) none is slower: by cycles, fib −52%, cart and
collatz −12%, mandelbrot −5%, strings −3%, loop −1%, the others within
0.5%.

With `python3 bench/run.py --runs 7` on the same machine, pinned to one
core, `fib` takes 0.028 s and CPython 3.14 0.091 s: 3.25 times as fast
(1.18 times before). Node.js takes 0.039 s.

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
