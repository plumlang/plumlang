/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The WebAssembly build's exports (make wasm; built only by Emscripten).
 * JavaScript calls these through src/wasm/api.js, which is the API
 * embedders use (docs/playground.md).
 *
 * A run's output and errors go to JavaScript as they are written, a line
 * at a time, through Module._plumEmit(stream, bytes): 1 for standard
 * output, 2 for standard error. Standard input is always at its end, so
 * ask() and io.read_line() give none.
 */
#define _GNU_SOURCE /* fopencookie */

#include "plum/version.h"
#include "util/alloc.h"
#include "wasm/playground.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* What JavaScript calls (api.js). */
int plum_wasm_run(const char *source, size_t len, double max_steps, double max_memory,
                  double max_sleep);
char *plum_wasm_check(const char *source, size_t len);
char *plum_wasm_format(const char *source, size_t len);
void plum_wasm_free(void *p);
const char *plum_wasm_version(void);

/* clang-format off */
EM_JS(void, plum_wasm_emit, (int stream, const char *bytes, size_t len), {
    Module["_plumEmit"](stream, HEAPU8.subarray(bytes, bytes + len));
})
/* clang-format on */

static ssize_t write_out(void *cookie, const char *buf, size_t size) {
    (void)cookie;
    plum_wasm_emit(1, buf, size);
    return (ssize_t)size;
}

static ssize_t write_err(void *cookie, const char *buf, size_t size) {
    (void)cookie;
    plum_wasm_emit(2, buf, size);
    return (ssize_t)size;
}

/* A stream whose bytes go to JavaScript, a line at a time. */
static FILE *to_js(ssize_t (*write)(void *, const char *, size_t)) {
    cookie_io_functions_t io = {0};
    io.write = write;
    FILE *f = fopencookie(NULL, "w", io);
    if (f != NULL) {
        setvbuf(f, NULL, _IOLBF, 4096);
    }
    return f;
}

/* Run `len` bytes of source; limits of 0 take the defaults
 * (src/wasm/playground.h), a max_sleep below 0 refuses sleeping. */
EMSCRIPTEN_KEEPALIVE int plum_wasm_run(const char *source, size_t len, double max_steps,
                                       double max_memory, double max_sleep) {
    FILE *out = to_js(write_out);
    FILE *err = to_js(write_err);
    if (out == NULL || err == NULL) {
        return 1;
    }
    PlumPlaygroundLimits limits = {(uint64_t)max_steps, (int64_t)max_memory, max_sleep};
    int code = plum_playground_run(source, len, &limits, out, err);
    fclose(out);
    fclose(err);
    return code;
}

/* JSON (plum_wasm_free it): see plum_playground_check and _format. */
EMSCRIPTEN_KEEPALIVE char *plum_wasm_check(const char *source, size_t len) {
    return plum_playground_check(source, len);
}

EMSCRIPTEN_KEEPALIVE char *plum_wasm_format(const char *source, size_t len) {
    return plum_playground_format(source, len);
}

EMSCRIPTEN_KEEPALIVE void plum_wasm_free(void *p) {
    plum_free(p);
}

EMSCRIPTEN_KEEPALIVE const char *plum_wasm_version(void) {
    return PLUM_VERSION;
}
