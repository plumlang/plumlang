/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The `plum fmt` command:
 *
 *   plum fmt <file or directory>...   format in place (directories: every
 *                                     .plum file under them)
 *   plum fmt --check <paths>...       change nothing; list the files that
 *                                     would change, and exit 1 if any would
 *   plum fmt --stdout <paths>...      write the formatted text to standard
 *                                     output instead
 *   plum fmt --stdin                  standard input to standard output
 *   plum fmt -                        the same
 *
 * A file with syntax errors is reported and left as it is (exit 1).
 */
#include "fmt/format.h"

#include "module/loader.h"
#include "stdlib/platform.h"
#include "util/alloc.h"
#include "util/array.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

enum {
    EXIT_FAILED = 1,  /* syntax errors, or --check found changes */
    EXIT_USAGE = 2,   /* the command was used wrongly */
    EXIT_NOINPUT = 3, /* a file could not be read or written */
};

/* Directories nested deeper than this are left alone (a symlink loop). */
#define MAX_DEPTH 64

typedef struct FmtRun {
    bool check;
    bool to_stdout;
    int status;
} FmtRun;

static void fail(FmtRun *run, int status) {
    if (status > run->status) {
        run->status = status;
    }
}

/* Format one source text that came from `path` ("<stdin>" for --stdin or -).
 * With syntax errors nothing goes to standard output. */
static void format_text(FmtRun *run, const char *path, const char *text, size_t len,
                        bool write_back) {
    PlumSource source = {path, text, len};
    PlumDiagList diags;
    plum_diag_init(&diags, &source);
    size_t out_len = 0;
    char *out = plum_format(&source, &diags, &out_len);
    if (out == NULL) {
        plum_diag_print(&diags, stderr, plum_diag_use_color(stderr));
        fprintf(stderr, "plum: %s has syntax errors; it was not formatted\n", path);
        fail(run, EXIT_FAILED);
        plum_diag_free(&diags);
        return;
    }
    plum_diag_free(&diags);
    bool same = out_len == len && memcmp(out, text, len) == 0;
    if (run->check) {
        if (!same) {
            printf("%s\n", path);
            fail(run, EXIT_FAILED);
        }
    } else if (run->to_stdout || !write_back) {
        fwrite(out, 1, out_len, stdout);
    } else if (!same) {
        FILE *file = fopen(path, "wb");
        bool ok = file != NULL && fwrite(out, 1, out_len, file) == out_len;
        int err = errno;
        if (file != NULL && fclose(file) != 0) {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "plum: error: cannot write '%s': %s\n", path, strerror(err));
            fail(run, EXIT_NOINPUT);
        }
    }
    plum_free(out);
}

static void format_file(FmtRun *run, const char *path) {
    size_t len = 0;
    int err = 0;
    char *text = plum_read_file(path, &len, &err);
    if (text == NULL) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", path, strerror(err));
        fail(run, EXIT_NOINPUT);
        return;
    }
    format_text(run, path, text, len, true);
    free(text);
}

static void format_stdin(FmtRun *run) {
    PLUM_ARRAY(char) text = {0};
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        plum_array_reserve(&text, n);
        memcpy(text.items + text.len, chunk, n);
        text.len += n;
    }
    plum_array_push(&text, '\0');
    format_text(run, "<stdin>", text.items, text.len - 1, false);
    plum_array_free(&text);
}

typedef PLUM_ARRAY(char *) Names;

static void add_name(void *ctx, const char *name) {
    Names *names = ctx;
    plum_array_push(names, plum_strdup(name));
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool is_plum_file(const char *name) {
    size_t n = strlen(name);
    return n > 5 && strcmp(name + n - 5, ".plum") == 0;
}

/* Every .plum file under `dir`, in name order; hidden entries are skipped. */
static void format_dir(FmtRun *run, const char *dir, int depth) {
    if (depth > MAX_DEPTH) {
        return;
    }
    Names names = {0};
    int err = plum_os_list_dir(dir, add_name, &names);
    if (err != 0) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", dir, strerror(err));
        fail(run, EXIT_NOINPUT);
    }
    if (names.len > 1) {
        qsort(names.items, names.len, sizeof(char *), compare_names);
    }
    size_t dlen = strlen(dir);
    while (dlen > 1 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) {
        dlen--;
    }
    for (size_t i = 0; i < names.len; i++) {
        const char *name = names.items[i];
        if (name[0] != '.') {
            size_t size = plum_add_size(plum_add_size(dlen, strlen(name)), 2);
            char *path = plum_alloc(size);
            snprintf(path, size, "%.*s/%s", (int)dlen, dir, name);
            if (plum_os_is_dir(path)) {
                format_dir(run, path, depth + 1);
            } else if (is_plum_file(name)) {
                format_file(run, path);
            }
            plum_free(path);
        }
        plum_free(names.items[i]);
    }
    plum_array_free(&names);
}

int plum_fmt_command(int argc, char **argv) {
    FmtRun run = {false, false, 0};
    int paths = 0;
    bool from_stdin = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            run.check = true;
        } else if (strcmp(argv[i], "--stdout") == 0) {
            run.to_stdout = true;
        } else if (strcmp(argv[i], "--stdin") == 0) {
            from_stdin = true;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr,
                    "plum: error: unknown option '%s'\n"
                    "  hint: plum fmt [--check] [--stdout] <files or directories...>, "
                    "or plum fmt --stdin\n",
                    argv[i]);
            return EXIT_USAGE;
        } else {
            paths++;
        }
    }
    if (from_stdin && paths > 0) {
        fputs("plum: error: `plum fmt --stdin` formats standard input and takes no paths\n"
              "  hint: plum fmt --stdin < hello.plum, or plum fmt hello.plum\n",
              stderr);
        return EXIT_USAGE;
    }
    if (paths == 0 && !from_stdin) {
        fputs("plum: error: `plum fmt` needs files or directories to format\n"
              "  hint: plum fmt hello.plum, plum fmt . (every .plum file), or plum fmt --stdin "
              "(standard input)\n",
              stderr);
        return EXIT_USAGE;
    }
#ifdef _WIN32
    /* The text as it is: no \r added before each \n, or taken away. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (from_stdin) {
        format_stdin(&run);
    }
    for (int i = 0; i < argc; i++) {
        const char *path = argv[i];
        if (path[0] == '-' && path[1] != '\0') {
            continue;
        }
        if (strcmp(path, "-") == 0) {
            format_stdin(&run);
        } else if (plum_os_is_dir(path)) {
            format_dir(&run, path, 0);
        } else {
            format_file(&run, path);
        }
    }
    fflush(stdout);
    return run.status;
}
