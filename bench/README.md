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
`+=` on a string copies it in Plumlang, CPython and Lua alike, so the
`textbuild` loop is kept short.

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
| cart | 0.398 s | 0.495 s | 1.24x | 0.337 s | 0.85x | 0.036 s | 0.09x |
| collatz | 1.314 s | 2.279 s | 1.73x | 0.759 s | 0.58x | 0.245 s | 0.19x |
| dispatch | 0.307 s | 0.284 s | 0.93x | 0.269 s | 0.88x | 0.031 s | 0.10x |
| fib | 0.065 s | 0.060 s | 0.92x | 0.050 s | 0.77x | 0.026 s | 0.40x |
| jsondoc | 0.241 s | 0.243 s | 1.01x | - | | 0.123 s | 0.51x |
| loop | 0.280 s | 0.741 s | 2.64x | 0.194 s | 0.69x | 0.049 s | 0.18x |
| mandelbrot | 0.358 s | 0.934 s | 2.61x | 0.199 s | 0.56x | 0.038 s | 0.11x |
| strings | 0.153 s | 0.175 s | 1.14x | 0.135 s | 0.88x | 0.049 s | 0.32x |
| textbuild | 0.331 s | 0.322 s | 0.97x | 0.318 s | 0.96x | 0.061 s | 0.18x |
| words | 0.318 s | 0.326 s | 1.02x | 0.194 s | 0.61x | 0.277 s | 0.87x |

Plumlang is ahead of CPython or within 8% of it on every benchmark. Lua's
interpreter is faster on all of them, by up to 1.8 times on float and
integer arithmetic (`mandelbrot`, `collatz`). Node.js compiles hot code to machine code
(V8's JIT), which an interpreter doesn't; `words`, where the time goes into
hashing and allocating strings, is where it is closest.

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

Each change was measured with `bench/run.py --runs 7` (the LTO and PGO
rows: `--runs 5`, Linux x86_64, GCC 15.2) and kept only if it helped.

| Change | Result | Kept |
|---|---|---|
| Typed `==` / `!=` for numbers (`EQ_INT`, `EQ_FLOAT`) instead of a generic comparison | collatz 1.41 s → 1.27 s, loop 0.30 s → 0.28 s | yes |
| Computed-goto dispatch (GCC/Clang) instead of `switch` | no difference (collatz 1.27 s → 1.32 s, within noise): GCC's jump table is already well predicted | no |
| Compiler-computed stack bound, so `push` skips its capacity check | no difference: the check is a perfectly predicted branch | no |
| Link-time optimisation alone (`-flto`, `make release-lto`) | mixed: jsondoc 0.236 s → 0.192 s and strings 0.150 s → 0.136 s (the stdlib's C inlined into the VM's calls), but cart, dispatch and loop 3–5% slower | only as part of PGO, and where PGO can't run |
| Profile-guided optimisation alone (`-fprofile-use`, no LTO) | collatz −11%, fib −9%, jsondoc −9%, strings −8%; dispatch and cart within noise | no: LTO + PGO does more |
| LTO + PGO (`make release-pgo`, trained by `tools/pgo-train.sh` on bench/ and examples/) | cart −17%, dispatch −17%, collatz −12%, fib −12%, jsondoc −11%, words −11%, strings −9%; loop, mandelbrot and textbuild unchanged; nothing slower | yes, for release binaries |

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
Linux riscv64 (built under emulation) gets LTO only. macOS gets no LTO
for now (arm64 gets PGO alone, x86_64 is cross-compiled and plain): with
Apple Clang, every LTO build (LTO alone, LTO + PGO, and the
instrumented LTO build) aborts in the network tests (`pointer being freed
was not allocated`), while the plain, PGO-only and ASan + UBSan builds
pass; that is still to be tracked down. Windows gets
MSVC's LTCG (`-DPLUM_LTO=ON`) and no PGO, as MSVC's needs its own runtime
DLL while training and the ARM64 build is cross-compiled.

Method calls cost what function calls do (`dispatch` and `fib` both run
level with Python): finding the method is one table lookup, and the time
goes into setting up each call's frame.

The run loop is now dominated by the number of instructions executed
(about 16 per iteration of Collatz's inner loop). Bigger gains need fewer
instructions per operation: superinstructions (`x += 1` as one
instruction) or register-based instructions. Those change the bytecode
format, so they are a design decision for later, measured with this suite.
