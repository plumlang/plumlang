/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The `plum` command.
 *
 *   plum                     help
 *   plum help                help
 *   plum version             version and build information
 *   plum run <file> [args]   run a program
 *   plum <file.plum> [args]  shorthand for `plum run`
 *   plum tokens <file>       print the tokens of a file (for debugging)
 *   plum ast <file>          print the syntax tree of a file (for debugging)
 *   plum check <file>        report every error without running the program
 *
 * Exit codes: 0 success, 1 the program failed, 2 wrong usage,
 * 3 the source file could not be read.
 */
#include "check/resolve.h"
#include "check/typecheck.h"
#include "diag/diag.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "plum/version.h"
#include "util/arena.h"
#include "util/fmt.h"
#include "util/intern.h"

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
          "  plum check <file>           Report every error without running\n"
          "  plum version                Show the version and build\n"
          "  plum tokens <file>          Print the tokens of a file (debugging)\n"
          "  plum ast <file>             Print the syntax tree of a file (debugging)\n"
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

typedef enum FrontStage {
    STAGE_TOKENS, /* plum tokens: print the tokens */
    STAGE_AST,    /* plum ast: print the syntax tree */
    STAGE_CHECK,  /* plum check, plum run: report errors only */
} FrontStage;

static int cmd_front(const char *name, FrontStage stage, int argc, char **argv);

static int cmd_run(int argc, char **argv) {
    if (argc < 1) {
        fputs("plum: error: `plum run` needs a file to run\n"
              "  hint: plum run hello.plum\n",
              stderr);
        return PLUM_EXIT_USAGE;
    }
    int status = cmd_front("run", STAGE_CHECK, 1, argv);
    if (status != PLUM_EXIT_OK) {
        return status;
    }
    /* The compiler and virtual machine arrive in roadmap stage 4. */
    fprintf(stderr,
            "plum: error: this build cannot run programs yet; '%s' has no errors\n"
            "  note: the compiler and virtual machine arrive in roadmap stage 4 (#36)\n",
            argv[0]);
    return PLUM_EXIT_FAILED;
}

static void print_token(const PlumToken *tok) {
    printf("%u:%u %s", (unsigned)tok->span.line, (unsigned)tok->span.column,
           plum_token_kind_name(tok->kind));
    switch (tok->kind) {
    case PLUM_TOK_INT:
        printf(" %lld", (long long)tok->as.i);
        break;
    case PLUM_TOK_FLOAT: {
        char buf[PLUM_FLOAT_BUF];
        printf(" %s", plum_format_float(tok->as.f, buf));
        break;
    }
    case PLUM_TOK_IDENT:
        printf(" %s", tok->as.name);
        break;
    case PLUM_TOK_STRING:
    case PLUM_TOK_STRING_HEAD:
    case PLUM_TOK_STRING_MID:
    case PLUM_TOK_STRING_TAIL:
    case PLUM_TOK_FORMAT:
        putchar(' ');
        plum_write_quoted(stdout, tok->as.str.chars, tok->as.str.len);
        break;
    default:
        break;
    }
    putchar('\n');
}

/* plum tokens / plum ast: run the front end on one file and print it. */
static int cmd_front(const char *name, FrontStage stage, int argc, char **argv) {
    if (argc != 1) {
        fprintf(stderr,
                "plum: error: `plum %s` needs one file\n"
                "  hint: plum %s hello.plum\n",
                name, name);
        return PLUM_EXIT_USAGE;
    }
    const char *path = argv[0];
    size_t len = 0;
    int err = 0;
    char *text = read_file(path, &len, &err);
    if (text == NULL) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", path, strerror(err));
        return PLUM_EXIT_NOINPUT;
    }

    PlumSource source = {path, text, len};
    PlumArena arena;
    PlumInterner names;
    PlumDiagList diags;
    PlumTokenList tokens = {0};
    plum_arena_init(&arena);
    plum_interner_init(&names);
    plum_diag_init(&diags, &source);

    plum_lex(&source, &arena, &names, &diags, &tokens);
    if (stage == STAGE_TOKENS) {
        for (size_t i = 0; i < tokens.len; i++) {
            print_token(&tokens.items[i]);
        }
    } else {
        PlumParser parser;
        plum_parser_init(&parser, &tokens, &arena, &names, &diags);
        PlumProgram program = plum_parse_program(&parser);
        if (stage == STAGE_AST) {
            plum_print_program(stdout, &program);
        } else if (diags.errors == 0) {
            /* Names are only resolved in a program that parsed, so a
             * syntax error doesn't also show up as unknown names. */
            plum_resolve(&program, &arena, &names, &diags);
            if (diags.errors == 0) {
                plum_typecheck(&program, &arena, &names, &diags);
            }
        }
    }
    fflush(stdout);
    plum_diag_print(&diags, stderr, plum_diag_use_color(stderr));
    int status = diags.errors > 0 ? PLUM_EXIT_FAILED : PLUM_EXIT_OK;

    plum_array_free(&tokens);
    plum_diag_free(&diags);
    plum_interner_free(&names);
    plum_arena_free(&arena);
    free(text);
    return status;
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
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        print_version();
        return PLUM_EXIT_OK;
    }
    if (strcmp(cmd, "tokens") == 0) {
        return cmd_front("tokens", STAGE_TOKENS, argc - 2, argv + 2);
    }
    if (strcmp(cmd, "ast") == 0) {
        return cmd_front("ast", STAGE_AST, argc - 2, argv + 2);
    }
    if (strcmp(cmd, "check") == 0) {
        return cmd_front("check", STAGE_CHECK, argc - 2, argv + 2);
    }
    if (strcmp(cmd, "run") == 0) {
        return cmd_run(argc - 2, argv + 2);
    }
    if (ends_with(cmd, ".plum")) {
        return cmd_run(argc - 1, argv + 1);
    }

    fprintf(stderr,
            "plum: error: unknown command '%s'\n  hint: run `plum help` to see the commands\n",
            cmd);
    return PLUM_EXIT_USAGE;
}
