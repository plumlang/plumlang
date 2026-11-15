# Benchmarks

Each `NAME.plum` here has the same program in Python under
`python/NAME.py`. `python3 bench/run.py` runs both (fastest of three runs)
and checks they print the same output before comparing times.

| Benchmark | Exercises |
|---|---|
| `fib` | function calls: fib(30), about 2.7 million calls |
| `loop` | a 20-million-step range loop with int arithmetic |
| `collatz` | while loops, `//` and `%`, comparisons |
| `mandelbrot` | float arithmetic in nested loops |
| `strings` | interpolation with formats, `len` |
| `dispatch` | 3 million method calls through a trait, and a method changing its object |

Results depend on the machine; compare runs on the same one. Build with
`make` (release, `-O2`) before measuring.

## Results

Linux x86_64, GCC 15.2 at `-O2`, Python 3.14.4; fastest of seven runs.
"Speed-up" is how many times faster Plumlang is.

| Benchmark | plum | python3 | Speed-up |
|---|---|---|---|
| collatz | 1.27 s | 2.28 s | 1.79x |
| dispatch | 0.288 s | 0.284 s | 0.99x |
| fib | 0.061 s | 0.059 s | 0.96x |
| loop | 0.276 s | 0.711 s | 2.58x |
| mandelbrot | 0.341 s | 0.860 s | 2.52x |
| strings | 0.137 s | 0.160 s | 1.17x |

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

Each change was measured with `bench/run.py --runs 7` and kept only if it
helped.

| Change | Result | Kept |
|---|---|---|
| Typed `==` / `!=` for numbers (`EQ_INT`, `EQ_FLOAT`) instead of a generic comparison | collatz 1.41 s → 1.27 s, loop 0.30 s → 0.28 s | yes |
| Computed-goto dispatch (GCC/Clang) instead of `switch` | no difference (collatz 1.27 s → 1.32 s, within noise): GCC's jump table is already well predicted | no |
| Compiler-computed stack bound, so `push` skips its capacity check | no difference: the check is a perfectly predicted branch | no |

Method calls cost what function calls do (`dispatch` and `fib` both run
level with Python): finding the method is one table lookup, and the time
goes into setting up each call's frame.

The run loop is now dominated by the number of instructions executed
(about 16 per iteration of Collatz's inner loop). Bigger gains need fewer
instructions per operation: superinstructions (`x += 1` as one
instruction) or register-based instructions. Those change the bytecode
format, so they are a design decision for later, measured with this suite.
