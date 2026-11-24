/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The `plum` command.
 *
 *   plum                     the REPL on a terminal; help otherwise
 *   plum repl                the REPL (also with piped input)
 *   plum help                help
 *   plum version             version and build information
 *   plum run <file> [args]   run a program
 *   plum <file.plum> [args]  shorthand for `plum run`
 *   plum tokens <file>       print the tokens of a file (for debugging)
 *   plum ast <file>          print the syntax tree of a file (for debugging)
 *   plum check <file>        report every error without running the program
 *   plum dis <file>          print the compiled bytecode (for debugging)
 *   plum fmt <paths>         format source files (src/fmt/command.c)
 *   plum add, remove, install, package, publish, search
 *                            the package commands (src/cli/orchard.c)
 *   plum lsp                 the language server, for editors (src/lsp/)
 *
 * Exit codes: 0 success, 1 the program failed, 2 wrong usage,
 * 3 the source file could not be read.
 */
#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"
#include "cli/orchard.h"
#include "compiler/chunk.h"
#include "compiler/compiler.h"
#include "diag/diag.h"
#include "fmt/format.h"
#include "lexer/lexer.h"
#include "lsp/lsp.h"
#include "module/loader.h"
#include "orchard/manifest.h"
#include "parser/parser.h"
#include "plum/version.h"
#include "repl/line.h"
#include "repl/repl.h"
#include "stdlib/native.h"
#include "stdlib/platform.h"
#include "util/arena.h"
#include "util/fmt.h"
#include "util/intern.h"
#include "vm/vm.h"

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
          "    --check-leaks             ...and report objects left alive in cycles\n"
          "    --mem-stats               ...and print how much memory it used\n"
          "  plum <file.plum> [args...]  Same as `plum run`\n"
          "  plum check <file>           Report every error without running\n"
          "  plum fmt <paths...>         Format files, and the .plum files in directories\n"
          "    --check                   ...change nothing; list files that would change\n"
          "  plum fmt --stdin            Format standard input to standard output (or `-`)\n"
          "  plum repl                   Type and run code interactively (also plain `plum`)\n"
          "  plum lsp                    The language server, for editors (docs/lsp.md)\n"
          "  plum version                Show the version and build\n"
          "  plum tokens <file>          Print the tokens of a file (debugging)\n"
          "  plum ast <file>             Print the syntax tree of a file (debugging)\n"
          "  plum dis <file>             Print the compiled bytecode (debugging)\n"
          "  plum stdlib [module]        List the standard library's functions\n"
          "  plum help                   Show this help\n"
          "\n"
          "Packages (Orchard; run in the project's directory):\n"
          "  plum add <name>[@range]     Add a dependency to plum.toml, lock and install it\n"
          "  plum remove <name>          Remove a dependency\n"
          "  plum install                Install what plum.lock lists (or resolve and write it)\n"
          "  plum package                Check the package and write NAME-VERSION.plumpkg\n"
          "  plum publish [--tag TAG]    Package, then print the link to publish it on Orchard\n"
          "  plum search <text>          Search Orchard's packages\n"
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

typedef enum FrontStage {
    STAGE_TOKENS, /* plum tokens: print the tokens */
    STAGE_AST,    /* plum ast: print the syntax tree */
    STAGE_CHECK,  /* plum check: report errors only */
    STAGE_DIS,    /* plum dis: compile and print the bytecode */
    STAGE_RUN,    /* plum run: compile and run */
} FrontStage;

static int cmd_front(const char *name, FrontStage stage, int argc, char **argv,
                     const PlumRunOptions *options);

static int cmd_run(int argc, char **argv) {
    PlumRunOptions options = {0};
    /* Options for plum itself come before the file. */
    while (argc > 0 && argv[0][0] == '-' && argv[0][1] == '-') {
        if (strcmp(argv[0], "--check-leaks") == 0) {
            options.check_leaks = true;
        } else if (strcmp(argv[0], "--mem-stats") == 0) {
            options.mem_stats = true;
        } else {
            fprintf(stderr,
                    "plum: error: unknown option '%s'\n"
                    "  hint: plum run [--check-leaks] [--mem-stats] hello.plum [args...]\n",
                    argv[0]);
            return PLUM_EXIT_USAGE;
        }
        argc--;
        argv++;
    }
    if (argc < 1) {
        fputs("plum: error: `plum run` needs a file to run\n"
              "  hint: plum run hello.plum\n",
              stderr);
        return PLUM_EXIT_USAGE;
    }
    /* Arguments after the file are the program's own (os.args). */
    options.argc = argc;
    options.argv = argv;
    return cmd_front("run", STAGE_RUN, 1, argv, &options);
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

/* plum tokens, ast, check, dis and run: the pipeline up to `stage`. */
/* plum tokens and ast: one file, up to `stage`. */
static int cmd_one_file(const char *path, FrontStage stage) {
    size_t len = 0;
    int err = 0;
    char *text = plum_read_file(path, &len, &err);
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
        plum_print_program(stdout, &program);
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

/* plum check, dis and run: the program and every module it imports. */
static int cmd_front(const char *name, FrontStage stage, int argc, char **argv,
                     const PlumRunOptions *options) {
    if (argc != 1) {
        fprintf(stderr,
                "plum: error: `plum %s` needs one file\n"
                "  hint: plum %s hello.plum\n",
                name, name);
        return PLUM_EXIT_USAGE;
    }
    if (stage == STAGE_TOKENS || stage == STAGE_AST) {
        return cmd_one_file(argv[0], stage);
    }
    char *entry = NULL;
    const char *path = argv[0];
    if (plum_os_is_dir(path)) {
        entry = plum_package_entry(path, stderr);
        if (entry == NULL) {
            return PLUM_EXIT_NOINPUT;
        }
        path = entry;
    }
    PlumModules modules;
    int err = plum_modules_load(&modules, path);
    if (err != 0) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", path, strerror(err));
        plum_free(entry);
        return PLUM_EXIT_NOINPUT;
    }

    /* Each module in order, imports first: one that fails stops the rest,
     * whose imports would only look wrong. */
    bool ok = !plum_modules_failed(&modules);
    for (size_t i = 0; ok && i < modules.count; i++) {
        PlumModule *m = modules.items[i];
        plum_resolve(&m->program, &modules.arena, &modules.names, &m->diags, &m->exports);
        if (m->diags.errors == 0) {
            plum_typecheck(&m->program, &modules.arena, &modules.names, &m->diags);
        }
        if (m->diags.errors == 0) {
            plum_check_flow(&m->program, &m->diags);
        }
        ok = m->diags.errors == 0;
    }
    PlumCompiled compiled = {0};
    if (ok && (stage == STAGE_DIS || stage == STAGE_RUN)) {
        PlumCompileModule *list = calloc(modules.count, sizeof *list);
        for (size_t i = 0; list != NULL && i < modules.count; i++) {
            PlumModule *m = modules.items[i];
            list[i].program = &m->program;
            list[i].diags = &m->diags;
            list[i].path = i + 1 < modules.count ? m->path : NULL;
        }
        if (list != NULL) {
            compiled = plum_compile_modules(list, modules.count, &modules.names);
        }
        free(list);
        if (compiled.main != NULL && stage == STAGE_DIS) {
            plum_disassemble(stdout, compiled.main);
        }
    }
    fflush(stdout);
    /* Warnings and errors come before anything the program prints. */
    plum_modules_print_diags(&modules, stderr, plum_diag_use_color(stderr));
    int status = plum_modules_failed(&modules) ? PLUM_EXIT_FAILED : PLUM_EXIT_OK;
    if (compiled.main != NULL && stage == STAGE_RUN && status == PLUM_EXIT_OK) {
        fflush(stderr);
        PlumModule *program = modules.items[modules.count - 1];
        status = plum_vm_run(&compiled, &program->source, options, stdout, stderr);
    }
    plum_compiled_free(&compiled);
    plum_modules_free(&modules);
    plum_free(entry);
    return status;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* A module's reference: plain text, or the Markdown table docs/stdlib uses. */
static void print_module(const PlumNativeModule *m, bool markdown) {
    if (markdown) {
        printf("| Name | Signature | What it does |\n|---|---|---|\n");
        for (size_t i = 0; i < m->function_count; i++) {
            const PlumNativeFunction *f = &m->functions[i];
            printf("| `%s` | `%s%s` | %s |\n", f->name, f->name, f->signature, f->doc);
        }
        for (size_t i = 0; i < m->constant_count; i++) {
            const PlumNativeConstant *k = &m->constants[i];
            printf("| `%s` | `%s: %s` | %s |\n", k->name, k->name, plum_native_constant_type(k),
                   k->doc);
        }
        return;
    }
    printf("%s: %s\n", m->name, m->doc);
    for (size_t i = 0; i < m->function_count; i++) {
        const PlumNativeFunction *f = &m->functions[i];
        char sig[160];
        snprintf(sig, sizeof sig, "%s%s", f->name, f->signature);
        printf("  %-48s %s\n", sig, f->doc);
    }
    for (size_t i = 0; i < m->constant_count; i++) {
        const PlumNativeConstant *k = &m->constants[i];
        char sig[96];
        snprintf(sig, sizeof sig, "%s: %s", k->name, plum_native_constant_type(k));
        printf("  %-48s %s\n", sig, k->doc);
    }
}

/* plum stdlib [--markdown] [module] */
static int cmd_stdlib(int argc, char **argv) {
    bool markdown = argc > 0 && strcmp(argv[0], "--markdown") == 0;
    if (markdown) {
        argc--;
        argv++;
    }
    if (argc > 0) {
        const PlumNativeModule *m = plum_native_module(argv[0]);
        if (m == NULL) {
            fprintf(stderr,
                    "plum: error: there is no standard module '%s'\n"
                    "  hint: plum stdlib lists them\n",
                    argv[0]);
            return PLUM_EXIT_USAGE;
        }
        print_module(m, markdown);
        return PLUM_EXIT_OK;
    }
    for (size_t i = 0; plum_native_module_at(i) != NULL; i++) {
        if (i > 0) {
            putchar('\n');
        }
        print_module(plum_native_module_at(i), markdown);
    }
    return PLUM_EXIT_OK;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        /* On a terminal, the REPL; with piped input, the help. */
        if (plum_line_input_is_terminal()) {
            return plum_repl_main();
        }
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
        return cmd_front("tokens", STAGE_TOKENS, argc - 2, argv + 2, NULL);
    }
    if (strcmp(cmd, "ast") == 0) {
        return cmd_front("ast", STAGE_AST, argc - 2, argv + 2, NULL);
    }
    if (strcmp(cmd, "dis") == 0) {
        return cmd_front("dis", STAGE_DIS, argc - 2, argv + 2, NULL);
    }
    if (strcmp(cmd, "stdlib") == 0) {
        return cmd_stdlib(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "check") == 0) {
        return cmd_front("check", STAGE_CHECK, argc - 2, argv + 2, NULL);
    }
    if (strcmp(cmd, "fmt") == 0) {
        return plum_fmt_command(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "lsp") == 0) {
        return plum_lsp_command(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "run") == 0) {
        return cmd_run(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "repl") == 0) {
        return plum_repl_main();
    }
    if (plum_orchard_is_command(cmd)) {
        return plum_orchard_command(argc, argv);
    }
    if (ends_with(cmd, ".plum")) {
        return cmd_run(argc - 1, argv + 1);
    }

    fprintf(stderr,
            "plum: error: unknown command '%s'\n  hint: run `plum help` to see the commands\n",
            cmd);
    return PLUM_EXIT_USAGE;
}
