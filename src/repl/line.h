/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * A small line editor for the REPL, with no dependencies: the cursor
 * keys, Home and End, Backspace and Delete, Ctrl-A/E/B/F/U/K/W/L/C/D/P/N,
 * and a history of earlier lines (up and down) kept in a file.
 *
 * On POSIX the terminal is put in raw mode with termios while a line is
 * read; on Windows the console's key events are read with
 * ReadConsoleInputW, and output uses its virtual terminal sequences. The
 * terminal is back in its usual mode whenever plum_line_read returns, so
 * the program run between lines reads and prints as it always does.
 */
#ifndef PLUM_REPL_LINE_H
#define PLUM_REPL_LINE_H

#include <stdbool.h>
#include <stddef.h>

/* At most this many lines are kept in the history (and its file). */
#define PLUM_HISTORY_MAX 1000

typedef enum PlumLineStatus {
    PLUM_LINE_OK,        /* a line was read */
    PLUM_LINE_EOF,       /* Ctrl-D on an empty line, or the input ended */
    PLUM_LINE_INTERRUPT, /* Ctrl-C: forget the line */
} PlumLineStatus;

typedef struct PlumLineEditor {
    char **history; /* oldest first, each malloc'd */
    size_t history_len;
    char *history_path; /* NULL: the history isn't saved */
} PlumLineEditor;

/* Are standard input and output both terminals (so lines can be edited)? */
bool plum_line_is_terminal(void);

/* Is standard input a terminal? */
bool plum_line_input_is_terminal(void);

/*
 * Start an editor, loading the history from `history_path` (NULL: keep
 * no history file).
 */
void plum_line_init(PlumLineEditor *ed, const char *history_path);
void plum_line_free(PlumLineEditor *ed);

/*
 * Show `prompt` and read a line with editing. On PLUM_LINE_OK, `*line`
 * is the text without its newline (malloc'd; the caller frees it).
 */
PlumLineStatus plum_line_read(PlumLineEditor *ed, const char *prompt, char **line);

/* The same without editing: print `prompt`, read a line from stdin. */
PlumLineStatus plum_line_read_plain(const char *prompt, char **line);

/* Add a line to the history (not an empty one, nor a repeat of the last)
 * and save the history file. */
void plum_line_add_history(PlumLineEditor *ed, const char *line);

/*
 * The history file's path (malloc'd), or NULL for none: $PLUM_HISTORY if
 * set (empty: none), else .plum_history in the home directory ($HOME, or
 * %USERPROFILE% on Windows).
 */
char *plum_line_history_path(void);

#endif /* PLUM_REPL_LINE_H */
