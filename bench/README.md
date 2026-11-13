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
