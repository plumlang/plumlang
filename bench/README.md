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

Results depend on the machine; compare runs on the same one. Build with
`make` (release, `-O2`) before measuring.
