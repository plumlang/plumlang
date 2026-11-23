<!-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 -->
# Fuzz targets

libFuzzer targets that feed `plum` arbitrary input, under AddressSanitizer
and UndefinedBehaviorSanitizer (issue #86). No input may crash, hang, leak
or corrupt memory; every failure must be a clear error.

| Target | Input | Through |
|---|---|---|
| `lexer` | source text | the lexer |
| `parser` | source text | lexer, parser, `plum ast` printer |
| `check` | source text | name resolution, type checking, flow checks, compiler, `plum dis` |
| `run` | source text | all of the above, then the VM: sandboxed, at most 200,000 steps and 256 MB |
| `json` | JSON text | `json.parse`, the `Json` methods, `json.stringify` (which must read back) |
| `format` | a value and a format | interpolation formats (`"{x:>10.2f}"`, spec 4.10) |
| `http` | network bytes | the `net.http` server (as requests), its client (as a response), `parse_url`, `parse_query` |

Run them with clang (Linux or macOS):

```bash
make fuzz                         # each target for 60 s
make fuzz FUZZ_TIME=600           # longer
make fuzz FUZZ_TARGETS="run json" # some of them
make fuzz FUZZ_CC=clang-18        # another clang
make fuzz-build                   # only build build/fuzz/fuzz-NAME
```

Seeds come from the repository's tests and examples (`corpus.sh`); what a
run finds is kept in `build/fuzz/corpus/NAME` for the next one. A failure
is saved in `build/fuzz/crashes/NAME/` and its log in
`build/fuzz/fuzz-NAME.log`. Reproduce one with
`build/fuzz/fuzz-NAME build/fuzz/crashes/NAME/crash-...`; then fix the
code, never the target, and add the input (made small) as a golden test
under `tests/lang`.

The `run` target uses `PlumRunOptions` limits that the command line does
not offer: `max_steps`, `max_memory` and `sandbox` (no files, network,
commands or sleeping), so a fuzzed program can neither touch the machine
nor run forever.
