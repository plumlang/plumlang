/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The REPL: `plum` on a terminal, or `plum repl`. Each entry (a line, or
 * several while a bracket is open or a line ends with an operator) is
 * checked, compiled and run at once, and what it declares stays for the
 * entries after it. An entry that is one expression prints its value and
 * type. See docs/repl.md.
 *
 * The session keeps one of each stage alive: the modules (whose arena
 * holds every entry's syntax tree and symbols until the session ends),
 * the names declared so far, one compiler (global slots, trees, methods)
 * and one VM (the globals' values).
 */
#ifndef PLUM_REPL_REPL_H
#define PLUM_REPL_REPL_H

/*
 * Run the REPL on standard input until it ends, :quit or exit(). Prompts
 * and the line editor are used when standard input is a terminal; piped
 * input is read line by line, without prompts. Returns the exit code: 0,
 * or the code given to exit().
 */
int plum_repl_main(void);

#endif /* PLUM_REPL_REPL_H */
