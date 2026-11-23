/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* termios, isatty and read are POSIX, and TIOCGWINSZ (the terminal's
 * width) is BSD; -std=c11 hides them unless asked for. */
#if !defined(_WIN32)
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#if !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "repl/line.h"

#include "util/alloc.h"
#include "util/array.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

/* Keys other than characters, above every Unicode code point. */
enum {
    KEY_NONE = -1, /* nothing to do (an unknown escape sequence) */
    KEY_EOF = -2,  /* the input ended */
    KEY_LEFT = 0x110000,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
};

#define KEY_CTRL(c) ((c) & 0x1f)

/* ------------------------------------------------------------------ */
/* The terminal                                                        */
/* ------------------------------------------------------------------ */

typedef struct Term {
#if defined(_WIN32)
    HANDLE in;
    HANDLE out;
    DWORD in_mode;
    DWORD out_mode;
    UINT out_cp;
    WCHAR high; /* the first half of a surrogate pair */
#else
    struct termios saved;
#endif
} Term;

#if defined(_WIN32)

/* A console, not a pipe, a file or NUL (which _isatty calls a terminal). */
static bool is_console(DWORD which) {
    DWORD mode;
    return GetConsoleMode(GetStdHandle(which), &mode) != 0;
}

bool plum_line_input_is_terminal(void) {
    return is_console(STD_INPUT_HANDLE);
}

bool plum_line_is_terminal(void) {
    return is_console(STD_INPUT_HANDLE) && is_console(STD_OUTPUT_HANDLE);
}

/* Raw input: key by key, no echo, Ctrl-C as a key; and output that
 * understands the cursor sequences, in UTF-8. */
static bool term_raw(Term *t) {
    t->in = GetStdHandle(STD_INPUT_HANDLE);
    t->out = GetStdHandle(STD_OUTPUT_HANDLE);
    t->high = 0;
    if (!GetConsoleMode(t->in, &t->in_mode) || !GetConsoleMode(t->out, &t->out_mode)) {
        return false;
    }
    DWORD raw =
        t->in_mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(t->in, raw)) {
        return false;
    }
    if (!SetConsoleMode(t->out, t->out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        SetConsoleMode(t->in, t->in_mode); /* a console too old for the sequences */
        return false;
    }
    t->out_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    return true;
}

static void term_restore(Term *t) {
    SetConsoleMode(t->in, t->in_mode);
    SetConsoleMode(t->out, t->out_mode);
    SetConsoleOutputCP(t->out_cp);
}

static size_t term_width(Term *t) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(t->out, &info)) {
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        if (w > 0) {
            return (size_t)w;
        }
    }
    return 80;
}

static void term_write(Term *t, const char *s, size_t n) {
    while (n > 0) {
        DWORD done = 0;
        DWORD chunk = n > 4096 ? 4096 : (DWORD)n;
        if (!WriteConsoleA(t->out, s, chunk, &done, NULL) || done == 0) {
            return;
        }
        s += done;
        n -= done;
    }
}

static long term_key(Term *t) {
    for (;;) {
        INPUT_RECORD rec;
        DWORD n = 0;
        if (!ReadConsoleInputW(t->in, &rec, 1, &n) || n == 0) {
            return KEY_EOF;
        }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
            continue;
        }
        const KEY_EVENT_RECORD *k = &rec.Event.KeyEvent;
        switch (k->wVirtualKeyCode) {
        case VK_LEFT:
            return KEY_LEFT;
        case VK_RIGHT:
            return KEY_RIGHT;
        case VK_UP:
            return KEY_UP;
        case VK_DOWN:
            return KEY_DOWN;
        case VK_HOME:
            return KEY_HOME;
        case VK_END:
            return KEY_END;
        case VK_DELETE:
            return KEY_DELETE;
        default:
            break;
        }
        WCHAR ch = k->uChar.UnicodeChar;
        if (ch == 0) {
            continue; /* Shift, Ctrl, ... on their own */
        }
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            t->high = ch;
            continue;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF) {
            if (t->high == 0) {
                continue;
            }
            long cp = 0x10000 + (((long)t->high - 0xD800) << 10) + ((long)ch - 0xDC00);
            t->high = 0;
            return cp;
        }
        return (long)ch;
    }
}

#else

bool plum_line_input_is_terminal(void) {
    return isatty(STDIN_FILENO) != 0;
}

bool plum_line_is_terminal(void) {
    return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
}

/* Raw mode: byte by byte, no echo, Ctrl-C and Ctrl-D as keys. Typed-ahead
 * input (a pasted block) is kept, not flushed. */
static bool term_raw(Term *t) {
    if (tcgetattr(STDIN_FILENO, &t->saved) != 0) {
        return false;
    }
    struct termios raw = t->saved;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) == 0;
}

static void term_restore(Term *t) {
    tcsetattr(STDIN_FILENO, TCSADRAIN, &t->saved);
}

static size_t term_width(Term *t) {
    (void)t;
#if defined(TIOCGWINSZ)
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
#endif
    return 80;
}

static void term_write(Term *t, const char *s, size_t n) {
    (void)t;
    while (n > 0) {
        ssize_t done = write(STDOUT_FILENO, s, n);
        if (done < 0 && errno == EINTR) {
            continue;
        }
        if (done <= 0) {
            return;
        }
        s += done;
        n -= (size_t)done;
    }
}

static int read_byte(void) {
    unsigned char c;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1) {
            return c;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
}

/* The key an escape sequence names: ESC [ A, ESC [ 3 ~, ESC O H, ... */
static long escape_key(void) {
    int a = read_byte();
    if (a < 0) {
        return KEY_EOF;
    }
    if (a != '[' && a != 'O') {
        return KEY_NONE; /* Alt and a key */
    }
    int b = read_byte();
    if (b < 0) {
        return KEY_EOF;
    }
    int number = 0;
    if (a == '[' && b >= '0' && b <= '9') {
        /* ESC [ n ~, or ESC [ 1 ; 5 C (with Ctrl or Shift held) */
        while (b >= '0' && b <= '9') {
            number = number * 10 + (b - '0');
            b = read_byte();
        }
        while (b == ';' || (b >= '0' && b <= '9')) {
            b = read_byte();
        }
        if (b < 0) {
            return KEY_EOF;
        }
        if (b == '~') {
            switch (number) {
            case 1:
            case 7:
                return KEY_HOME;
            case 4:
            case 8:
                return KEY_END;
            case 3:
                return KEY_DELETE;
            default:
                return KEY_NONE;
            }
        }
    }
    switch (b) {
    case 'A':
        return KEY_UP;
    case 'B':
        return KEY_DOWN;
    case 'C':
        return KEY_RIGHT;
    case 'D':
        return KEY_LEFT;
    case 'H':
        return KEY_HOME;
    case 'F':
        return KEY_END;
    default:
        return KEY_NONE;
    }
}

static long term_key(Term *t) {
    (void)t;
    int c = read_byte();
    if (c < 0) {
        return KEY_EOF;
    }
    if (c == 27) {
        return escape_key();
    }
    if (c < 0x80) {
        return c;
    }
    /* UTF-8: the lead byte says how many bytes follow */
    int more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (more < 0) {
        return KEY_NONE;
    }
    long cp = c & (0x3F >> more);
    for (int i = 0; i < more; i++) {
        int next = read_byte();
        if (next < 0) {
            return KEY_EOF;
        }
        if ((next & 0xC0) != 0x80) {
            return KEY_NONE;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    return cp;
}

#endif

/* ------------------------------------------------------------------ */
/* The line being edited                                               */
/* ------------------------------------------------------------------ */

typedef PLUM_ARRAY(char) Text;

typedef struct Line {
    Text text;
    size_t pos; /* the cursor, a byte offset at the start of a character */
} Line;

static bool is_continuation(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

static size_t prev_char(const Line *l, size_t pos) {
    if (pos == 0) {
        return 0;
    }
    pos--;
    while (pos > 0 && is_continuation(l->text.items[pos])) {
        pos--;
    }
    return pos;
}

static size_t next_char(const Line *l, size_t pos) {
    if (pos >= l->text.len) {
        return l->text.len;
    }
    pos++;
    while (pos < l->text.len && is_continuation(l->text.items[pos])) {
        pos++;
    }
    return pos;
}

/* The columns `n` bytes of UTF-8 take: one per character. */
static size_t columns(const char *s, size_t n) {
    size_t cols = 0;
    for (size_t i = 0; i < n; i++) {
        cols += !is_continuation(s[i]);
    }
    return cols;
}

static void text_insert(Text *t, size_t at, const char *s, size_t n) {
    if (n == 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        plum_array_push(t, '\0');
    }
    memmove(t->items + at + n, t->items + at, t->len - n - at);
    memcpy(t->items + at, s, n);
}

static void text_delete(Text *t, size_t from, size_t to) {
    if (from == to) {
        return;
    }
    memmove(t->items + from, t->items + to, t->len - to);
    t->len -= to - from;
}

static void text_set(Line *l, const char *s) {
    l->text.len = 0;
    text_insert(&l->text, 0, s, strlen(s));
    l->pos = l->text.len;
}

static size_t encode_utf8(long cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void append(Text *out, const char *s, size_t n) {
    text_insert(out, out->len, s, n);
}

/*
 * Redraw the prompt and the line on the terminal's current row. A line
 * wider than the terminal scrolls sideways to keep the cursor in view.
 */
static void refresh(Term *t, const char *prompt, const Line *l) {
    size_t prompt_cols = columns(prompt, strlen(prompt));
    size_t width = term_width(t);
    size_t room = width > prompt_cols + 1 ? width - prompt_cols - 1 : 1;
    size_t cursor = columns(l->text.items, l->pos);
    size_t skip = cursor >= room ? cursor - room + 1 : 0;
    size_t from = 0;
    for (size_t i = 0; i < skip; i++) {
        from = next_char(l, from);
    }
    size_t to = from;
    for (size_t i = 0; i < room && to < l->text.len; i++) {
        to = next_char(l, to);
    }
    Text out = {0};
    append(&out, "\r", 1);
    append(&out, prompt, strlen(prompt));
    append(&out, l->text.items + from, to - from);
    append(&out, "\x1b[0K\r", 5);
    size_t col = prompt_cols + cursor - skip;
    if (col > 0) {
        char move[32];
        int n = snprintf(move, sizeof move, "\x1b[%zuC", col);
        append(&out, move, (size_t)n);
    }
    term_write(t, out.items, out.len);
    plum_array_free(&out);
}

/* Up and down through the history; `index` == history_len is the line
 * being written, kept in `draft` while an older one is shown. */
static void history_move(PlumLineEditor *ed, Line *l, size_t *index, char **draft, bool up) {
    if (up) {
        if (*index == 0) {
            return;
        }
        if (*index == ed->history_len) {
            plum_free(*draft);
            *draft = plum_strndup(l->text.items, l->text.len);
        }
        (*index)--;
        text_set(l, ed->history[*index]);
        return;
    }
    if (*index >= ed->history_len) {
        return;
    }
    (*index)++;
    text_set(l, *index == ed->history_len ? (*draft != NULL ? *draft : "") : ed->history[*index]);
}

/* Ctrl-W: the word before the cursor, and the spaces after it. */
static void delete_word(Line *l) {
    size_t end = l->pos;
    size_t start = end;
    while (start > 0 && l->text.items[start - 1] == ' ') {
        start--;
    }
    while (start > 0 && l->text.items[start - 1] != ' ') {
        start--;
    }
    text_delete(&l->text, start, end);
    l->pos = start;
}

static PlumLineStatus edit(PlumLineEditor *ed, Term *t, const char *prompt, Line *l) {
    size_t index = ed->history_len;
    char *draft = NULL;
    PlumLineStatus status = PLUM_LINE_OK;
    refresh(t, prompt, l);
    for (;;) {
        long key = term_key(t);
        bool done = false;
        switch (key) {
        case KEY_EOF:
            status = PLUM_LINE_EOF;
            done = true;
            break;
        case '\r':
        case '\n':
            done = true;
            break;
        case KEY_CTRL('C'):
            term_write(t, "^C", 2);
            status = PLUM_LINE_INTERRUPT;
            done = true;
            break;
        case KEY_CTRL('D'):
            if (l->text.len == 0) {
                status = PLUM_LINE_EOF;
                done = true;
            } else if (l->pos < l->text.len) {
                text_delete(&l->text, l->pos, next_char(l, l->pos));
            }
            break;
        case KEY_DELETE:
            if (l->pos < l->text.len) {
                text_delete(&l->text, l->pos, next_char(l, l->pos));
            }
            break;
        case 127:
        case KEY_CTRL('H'):
            if (l->pos > 0) {
                size_t from = prev_char(l, l->pos);
                text_delete(&l->text, from, l->pos);
                l->pos = from;
            }
            break;
        case KEY_CTRL('A'):
        case KEY_HOME:
            l->pos = 0;
            break;
        case KEY_CTRL('E'):
        case KEY_END:
            l->pos = l->text.len;
            break;
        case KEY_CTRL('B'):
        case KEY_LEFT:
            l->pos = prev_char(l, l->pos);
            break;
        case KEY_CTRL('F'):
        case KEY_RIGHT:
            l->pos = next_char(l, l->pos);
            break;
        case KEY_CTRL('P'):
        case KEY_UP:
            history_move(ed, l, &index, &draft, true);
            break;
        case KEY_CTRL('N'):
        case KEY_DOWN:
            history_move(ed, l, &index, &draft, false);
            break;
        case KEY_CTRL('U'):
            text_delete(&l->text, 0, l->pos);
            l->pos = 0;
            break;
        case KEY_CTRL('K'):
            l->text.len = l->pos;
            break;
        case KEY_CTRL('W'):
            delete_word(l);
            break;
        case KEY_CTRL('L'):
            term_write(t, "\x1b[H\x1b[2J", 7);
            break;
        case '\t':
            text_insert(&l->text, l->pos, "    ", 4);
            l->pos += 4;
            break;
        default:
            if (key >= 32 && key != 127 && key < 0x110000 && !(key >= 0xD800 && key <= 0xDFFF)) {
                char bytes[4];
                size_t n = encode_utf8(key, bytes);
                text_insert(&l->text, l->pos, bytes, n);
                l->pos += n;
            }
            break;
        }
        if (done) {
            break;
        }
        refresh(t, prompt, l);
    }
    term_write(t, "\r\n", 2);
    plum_free(draft);
    return status;
}

/* Without a terminal to edit on: the prompt, then a plain line. */
PlumLineStatus plum_line_read_plain(const char *prompt, char **line) {
    fputs(prompt, stdout);
    fflush(stdout);
    Text text = {0};
    int c;
    bool any = false;
    while ((c = getchar()) != EOF) {
        any = true;
        if (c == '\n') {
            break;
        }
        plum_array_push(&text, (char)c);
    }
    if (!any) {
        plum_array_free(&text);
        return PLUM_LINE_EOF;
    }
    if (text.len > 0 && text.items[text.len - 1] == '\r') {
        text.len--;
    }
    *line = plum_strndup(text.items != NULL ? text.items : "", text.len);
    plum_array_free(&text);
    return PLUM_LINE_OK;
}

PlumLineStatus plum_line_read(PlumLineEditor *ed, const char *prompt, char **line) {
    *line = NULL;
    fflush(stdout);
    fflush(stderr);
    Term t;
    if (!term_raw(&t)) {
        return plum_line_read_plain(prompt, line);
    }
    Line l = {{0}, 0};
    plum_array_push(&l.text, '\0'); /* never a NULL buffer */
    l.text.len = 0;
    PlumLineStatus status = edit(ed, &t, prompt, &l);
    term_restore(&t);
    if (status == PLUM_LINE_OK) {
        *line = plum_strndup(l.text.items, l.text.len);
    }
    plum_array_free(&l.text);
    return status;
}

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

static void history_push(PlumLineEditor *ed, const char *line) {
    if (line[0] == '\0' ||
        (ed->history_len > 0 && strcmp(ed->history[ed->history_len - 1], line) == 0)) {
        return;
    }
    if (ed->history_len == PLUM_HISTORY_MAX) {
        plum_free(ed->history[0]);
        memmove(ed->history, ed->history + 1, (PLUM_HISTORY_MAX - 1) * sizeof(char *));
        ed->history_len--;
    }
    if (ed->history == NULL) {
        ed->history = plum_alloc_zeroed(PLUM_HISTORY_MAX, sizeof(char *));
    }
    ed->history[ed->history_len++] = plum_strdup(line);
}

static void history_save(const PlumLineEditor *ed) {
    if (ed->history_path == NULL) {
        return;
    }
    FILE *f = fopen(ed->history_path, "w");
    if (f == NULL) {
        return; /* the session goes on without it */
    }
    for (size_t i = 0; i < ed->history_len; i++) {
        fputs(ed->history[i], f);
        fputc('\n', f);
    }
    fclose(f);
}

static void history_load(PlumLineEditor *ed) {
    FILE *f = fopen(ed->history_path, "r");
    if (f == NULL) {
        return;
    }
    Text line = {0};
    int c;
    do {
        c = fgetc(f);
        if (c == EOF || c == '\n') {
            if (line.len > 0 && line.items[line.len - 1] == '\r') {
                line.len--;
            }
            plum_array_push(&line, '\0');
            history_push(ed, line.items);
            line.len = 0;
        } else {
            plum_array_push(&line, (char)c);
        }
    } while (c != EOF);
    plum_array_free(&line);
    fclose(f);
}

void plum_line_init(PlumLineEditor *ed, const char *history_path) {
    memset(ed, 0, sizeof *ed);
    if (history_path != NULL) {
        ed->history_path = plum_strdup(history_path);
        history_load(ed);
    }
}

void plum_line_free(PlumLineEditor *ed) {
    for (size_t i = 0; i < ed->history_len; i++) {
        plum_free(ed->history[i]);
    }
    plum_free(ed->history);
    plum_free(ed->history_path);
    memset(ed, 0, sizeof *ed);
}

void plum_line_add_history(PlumLineEditor *ed, const char *line) {
    size_t before = ed->history_len;
    const char *last = before > 0 ? ed->history[before - 1] : NULL;
    history_push(ed, line);
    if (ed->history_len != before || (before > 0 && ed->history[before - 1] != last)) {
        history_save(ed);
    }
}

char *plum_line_history_path(void) {
    const char *own = getenv("PLUM_HISTORY");
    if (own != NULL) {
        return own[0] != '\0' ? plum_strdup(own) : NULL;
    }
#if defined(_WIN32)
    const char *home = getenv("USERPROFILE");
    const char *name = "\\.plum_history";
#else
    const char *home = getenv("HOME");
    const char *name = "/.plum_history";
#endif
    if (home == NULL || home[0] == '\0') {
        return NULL;
    }
    size_t n = strlen(home) + strlen(name) + 1;
    char *path = plum_alloc(n);
    snprintf(path, n, "%s%s", home, name);
    return path;
}
