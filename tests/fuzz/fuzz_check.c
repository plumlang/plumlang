/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* Fuzz target: any bytes, as a program, through every check (names, types,
 * control flow) and, when they pass, the compiler (`plum dis`). */
#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    PlumRunOptions options = fuzz_run_options();
    fuzz_program(fuzz_text(data, size), size, FUZZ_COMPILE, &options);
    return 0;
}
