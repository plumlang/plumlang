/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* From loaded modules to a program: shared by plum check, dis, run and test. */
#ifndef PLUM_CLI_PIPELINE_H
#define PLUM_CLI_PIPELINE_H

#include "compiler/compiler.h"
#include "module/loader.h"

#include <stdbool.h>

/* Resolve, type-check and flow-check each module, imports first. A module
 * with errors stops the rest, whose imports would only look wrong. True
 * when no module has an error. */
bool plum_cli_check(PlumModules *modules);

/* Compile checked modules into one program; its main is NULL after
 * errors, which are in the modules' lists. */
PlumCompiled plum_cli_compile(PlumModules *modules);

#endif /* PLUM_CLI_PIPELINE_H */
