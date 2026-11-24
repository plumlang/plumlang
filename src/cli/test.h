/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* The test runner: plum test [paths...] [--filter TEXT] [--json] (docs/test.md). */
#ifndef PLUM_CLI_TEST_H
#define PLUM_CLI_TEST_H

/* Run it with the arguments after `test`. Returns the exit code: 0 when
 * every test passed (or there were none), 1 when one failed or a test file
 * has errors, 2 for wrong usage, 3 when a path could not be read. */
int plum_test_command(int argc, char **argv);

#endif /* PLUM_CLI_TEST_H */
