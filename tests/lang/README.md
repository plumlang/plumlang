# Golden tests

Each `NAME.plum` here is a program run by `make test` (through
`tests/golden.sh`) with `plum run`, from the repository root:

- `NAME.out` holds the expected standard output; the program must exit 0.
- `NAME.err` holds the expected standard error; the program must exit 1.

A first line `# pending: <reason>` marks a test that cannot pass until a
feature lands. It runs, is reported as pending, and doesn't fail the run;
once it passes, the run fails until the line is removed.

`make golden-update` rewrites expectation files from the actual output.
Review the diff before committing it.

The programs under `basics/`, `arith/`, `operators/`, `variables/`,
`control/` and `functions/` port the Python prototype's examples (v0.1 to
v0.10) to the syntax in `docs/spec.md`, with the prototype's bugs fixed:
integers print as integers, and `/` is the only operator that makes a
float.
