/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The `plum` command.
 *
 *   plum                     help
 *   plum help                help
 *   plum version             version and build information
 *   plum run <file> [args]   run a program
 *   plum <file.plum> [args]  shorthand for `plum run`
 *
 * Exit codes: 0 success, 1 the program failed, 2 wrong usage,
 * 3 the source file could not be read.
 */
#include "plum/version.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PLUM_EXIT_OK = 0,
    PLUM_EXIT_FAILED = 1,
    PLUM_EXIT_USAGE = 2,
    PLUM_EXIT_NOINPUT = 3,
};

static void print_help(FILE *out) {
    fputs("Plumlang " PLUM_VERSION " - a small, fast, memory-safe programming language\n"
          "\n"
          "Usage:\n"
          "  plum run <file> [args...]   Run a Plumlang program\n"
          "  plum <file.plum> [args...]  Same as `plum run`\n"
          "  plum version                Show the version and build\n"
          "  plum help                   Show this help\n"
          "\n"
          "Docs and source: https://github.com/plumlang/plumlang\n",
          out);
}

static void print_version(void) {
    printf("plum " PLUM_VERSION " (" PLUM_COMMIT ") " PLUM_OS "-" PLUM_ARCH ", " PLUM_CC "\n");
}

/*
 * Read a whole file into a NUL-terminated buffer the caller frees.
 * Returns NULL and sets *err to errno (or ENOMEM / EFBIG) on failure.
 */
static char *read_file(const char *path, size_t *len_out, int *err) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        *err = errno;
        return NULL;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        *err = ENOMEM;
        return NULL;
    }

    for (;;) {
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) {
            break;
        }
        if (len == cap - 1) {
            if (cap > ((size_t)-1) / 2) {
                free(buf);
                fclose(f);
                *err = EFBIG;
                return NULL;
            }
            char *grown = realloc(buf, cap * 2);
            if (grown == NULL) {
                free(buf);
                fclose(f);
                *err = ENOMEM;
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }
    }

    if (ferror(f)) {
        *err = errno != 0 ? errno : EIO;
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    buf[len] = '\0';
    *len_out = len;
    return buf;
}

static int cmd_run(int argc, char **argv) {
    if (argc < 1) {
        fputs("plum: error: `plum run` needs a file to run\n"
              "  hint: plum run hello.plum\n",
              stderr);
        return PLUM_EXIT_USAGE;
    }

    const char *path = argv[0];
    size_t len = 0;
    int err = 0;
    char *source = read_file(path, &len, &err);
    if (source == NULL) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", path, strerror(err));
        return PLUM_EXIT_NOINPUT;
    }

    /* The compiler and virtual machine arrive in roadmap stages 2 to 4. */
    fprintf(stderr,
            "plum: error: this build cannot run programs yet; it read '%s' (%zu bytes)\n"
            "  note: the compiler and virtual machine arrive in roadmap stages 2 to 4 (#36)\n",
            path, len);
    free(source);
    return PLUM_EXIT_FAILED;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_help(stdout);
        return PLUM_EXIT_OK;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_help(stdout);
        return PLUM_EXIT_OK;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 ||
        strcmp(cmd, "-v") == 0) {
        print_version();
        return PLUM_EXIT_OK;
    }
    if (strcmp(cmd, "run") == 0) {
        return cmd_run(argc - 2, argv + 2);
    }
    if (ends_with(cmd, ".plum")) {
        return cmd_run(argc - 1, argv + 1);
    }

    fprintf(stderr, "plum: error: unknown command '%s'\n  hint: run `plum help` to see the commands\n",
            cmd);
    return PLUM_EXIT_USAGE;
}
