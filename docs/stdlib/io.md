# io

Standard input, output and error. `drop` and `ask` cover most programs;
`io` is for output without `drop`'s spaces and newline, for errors, and
for reading all of the input.

```plum
seed io

io.write("Loading")
for i in 0..3 {
    io.write(".")
}
io.write("\n")
io.write_err("warning: no config file\n")

plant text = io.read_all()     # everything piped in
```

Output to standard output is buffered: `io.flush()` sends it now (reading
input flushes it first, so prompts appear).

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `write` | `write(text: str)` | write text to standard output, as it is |
| `write_err` | `write_err(text: str)` | write text to standard error, as it is |
| `flush` | `flush()` | send what's waiting in standard output now |
| `read_line` | `read_line() -> str?` | a line from standard input without its line break; none at the end |
| `read_all` | `read_all() -> str` | everything left on standard input |
<!-- reference:end -->
