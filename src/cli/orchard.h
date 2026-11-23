/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* The package commands: plum add, remove, install, package, publish, search. */
#ifndef PLUM_CLI_ORCHARD_H
#define PLUM_CLI_ORCHARD_H

#include <stdbool.h>

/* Is `name` one of the package commands? */
bool plum_orchard_is_command(const char *name);

/* Run one: argv[0] is the command's name, the rest its arguments. Returns
 * the exit code. */
int plum_orchard_command(int argc, char **argv);

#endif /* PLUM_CLI_ORCHARD_H */
