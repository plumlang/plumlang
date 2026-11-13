/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The io module: standard input, output and error, without drop's spaces
 * and newline. */
#include "stdlib/native.h"

#include "util/array.h"

#include <stdio.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

static bool io_write(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    fwrite(s->chars, 1, s->len, plum_native_out(n));
    *result = plum_none();
    return true;
}

static bool io_write_err(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    fflush(plum_native_out(n)); /* keep the two streams in order on a terminal */
    fwrite(s->chars, 1, s->len, plum_native_err(n));
    fflush(plum_native_err(n));
    *result = plum_none();
    return true;
}

static bool io_flush(ARGS) {
    (void)args;
    (void)argc;
    fflush(plum_native_out(n));
    *result = plum_none();
    return true;
}

/* A line of input without its line break (\n or \r\n); none at the end. */
static bool io_read_line(ARGS) {
    (void)args;
    (void)argc;
    fflush(plum_native_out(n));
    PLUM_ARRAY(char) line = {0};
    int ch;
    bool any = false;
    while ((ch = getchar()) != EOF) {
        any = true;
        if (ch == '\n') {
            break;
        }
        plum_array_push(&line, (char)ch);
    }
    if (!any) {
        *result = plum_none();
    } else {
        size_t len = line.len > 0 && line.items[line.len - 1] == '\r' ? line.len - 1 : line.len;
        *result = plum_str_value(line.items != NULL ? line.items : "", len);
    }
    plum_array_free(&line);
    return true;
}

/* Everything left on standard input. */
static bool io_read_all(ARGS) {
    (void)args;
    (void)argc;
    fflush(plum_native_out(n));
    PLUM_ARRAY(char) all = {0};
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        for (size_t i = 0; i < got; i++) {
            plum_array_push(&all, chunk[i]);
        }
    }
    *result = plum_str_value(all.items != NULL ? all.items : "", all.len);
    plum_array_free(&all);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"write", "(text: str)", io_write, "write text to standard output, as it is"},
    {"write_err", "(text: str)", io_write_err, "write text to standard error, as it is"},
    {"flush", "()", io_flush, "send what's waiting in standard output now"},
    {"read_line", "() -> str?", io_read_line,
     "a line from standard input without its line break; none at the end"},
    {"read_all", "() -> str", io_read_all, "everything left on standard input"},
};

const PlumNativeModule plum_module_io = {
    "io",      "standard input, output and error",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
