# Writing a standard module

A standard module is a table of C functions that Plumlang code calls.
This page shows how to add one, using a made-up `colour` module. Read
[CONTRIBUTING.md](../../CONTRIBUTING.md) and [AGENTS.md](../../AGENTS.md)
first: C11, no dependencies, warnings as errors, memory-safe.

## 1. The functions

Make `src/stdlib/colour.c`. A native function gets its arguments
(borrowed: don't release them), stores its result with a reference of
its own, and returns `true`; on failure it throws and returns `false`.

```c
/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* The colour module: colours as #rrggbb text. */
#include "stdlib/native.h"

#include <stdio.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* rgb(r, g, b) -> "#rrggbb" */
static bool c_rgb(ARGS) {
    (void)argc;
    for (int i = 0; i < 3; i++) {
        if (args[i].as.i < 0 || args[i].as.i > 255) {
            return plum_native_throw(n, PLUM_ERROR_VALUE, "rgb: %lld is out of 0 to 255",
                                     (long long)args[i].as.i);
        }
    }
    char text[8];
    snprintf(text, sizeof text, "#%02x%02x%02x", (int)args[0].as.i, (int)args[1].as.i,
             (int)args[2].as.i);
    *result = plum_str_value(text, 7);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"rgb", "(r: int, g: int, b: int) -> str", c_rgb, "a colour as #rrggbb text"},
};

const PlumNativeModule plum_module_colour = {
    "colour", "colours as #rrggbb text", functions, sizeof functions / sizeof functions[0],
    NULL, 0,
};
```

- **The signature string is the function's type.** The type checker reads
  it, so `colour.rgb(1, "x", 3)` is a compile error and your C code can
  trust its arguments: an `int` parameter always holds an int, a `str`
  parameter a string (`plum_as_string(args[i])`). Types: `int float str
  bool byte none Json`, `list[T]`, `map[K, V]`, `set[T]`, `T?`, and `num`
  for int-or-float (a `num` result is an int when every `num` argument
  is).
- **A parameter written `= default` may be left out;** the function then
  sees `none` in its place and applies the default itself.
- **Methods of a type** (like those of `str`) are a module whose functions
  take `self` first: see `src/stdlib/string.c`.
- **Constants** go in a `PlumNativeConstant` table (see `math.c`).

## 2. What a native function can use

All in `src/stdlib/native.h`:

| | |
|---|---|
| `plum_native_throw(n, PLUM_ERROR_VALUE, fmt, ...)` | throw a built-in error (`VALUE`, `PARSE`, `INDEX`, `KEY`, `IO`, `NET`, ...); returns false |
| `plum_str_value(chars, len)`, `plum_int`, `plum_float`, `plum_bool`, `plum_none` | make results |
| `plum_list_new`, `plum_list_push`, `plum_native_new_map` + `plum_table_put` | make lists and maps (maps through `plum_native_new_map`, so they hash with the program's key) |
| `plum_num(v)` | a `num` argument as a double |
| `plum_native_text(n, v)` | any value as text, through its `to_str` |
| `plum_native_random(n)` | random bits from the program's generator |
| `plum_native_wait_io`, `plum_native_call_state`, `plum_native_sleep` | for functions that wait (see `net.c`): other tasks run meanwhile |

Strings are bytes (UTF-8 by convention); `len()` counts characters,
`byte_len()` bytes. Use `plum_alloc` and friends (`src/util/alloc.h`)
for memory, never `malloc`, and check every bound.

## 3. Register it

- Declare `extern const PlumNativeModule plum_module_colour;` in
  `src/stdlib/native.h`.
- Add `&plum_module_colour` at the **end** of the list in
  `src/stdlib/registry.c` (a function's number depends on its module's
  place, so don't reorder).

The Makefile and CMake pick up new `.c` files by themselves. Now `seed
colour` works, and `plum stdlib colour` lists it.

## 4. Document it

Make `docs/stdlib/colour.md` with an introduction, examples, the errors
it throws, and an empty reference section:

```markdown
## Reference

<!-- reference:begin -->
<!-- reference:end -->
```

Run `make stdlib-doc` to fill the table from your signatures and doc
lines. Add the module to `docs/stdlib/README.md`'s table. `make test`
fails if a module has no page or its table is out of date.

## 5. Test it

Golden tests in `tests/lang/stdlib/` (see `tests/lang/README.md`):
`colour.plum` with its expected output in `colour.out`, and error cases
in `error_colour.plum` / `.err`. Write the program, run `make
golden-update`, and **read** the `.out` file it wrote: that's the
expectation now. Cover the edges: empty input, the largest and smallest
values, bad input, and what it throws.

```bash
make test             # unit, golden and docs checks
make sanitize test    # again under ASan + UBSan: no leaks, no undefined behaviour
make format           # clang-format 18.1.8 (pip install clang-format==18.1.8)
```

Add a line to `CHANGELOG.md` under `## Unreleased`, and open a pull
request that says what you added and how you tested it.

## Writing it in Plumlang instead

A module can also be written in Plumlang, like `net.http`: put it in
`src/stdlib/plum/` (`net_http.plum` is `net.http`), start it with a
`# module:` comment, and run `make stdlib-embed` to build it into the
binary. That suits code that mostly calls other modules; byte-level
work belongs in C.
