/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Fuzz target: any bytes, as a program, checked, compiled and run in the
 * sandbox with a step and memory budget (fuzz_run_options). */
#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static bool started;
    if (!started) {
        started = true;
        fuzz_stdin(NULL, 0); /* ask() and io.read_all() see the end of input */
    }
    PlumRunOptions options = fuzz_run_options();
    fuzz_program(fuzz_text(data, size), size, FUZZ_RUN, &options);
    return 0;
}
