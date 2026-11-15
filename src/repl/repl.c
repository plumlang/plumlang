/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "repl/repl.h"

#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"
#include "check/types.h"
#include "compiler/compiler.h"
#include "diag/diag.h"
#include "lexer/lexer.h"
#include "module/loader.h"
#include "plum/version.h"
#include "repl/line.h"
#include "util/alloc.h"
#include "util/array.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What diagnostics and stack traces call an entry typed at the prompt. */
#define ENTRY_PATH "<repl>"

typedef PLUM_ARRAY(char) Text;

/* Everything declared so far, and the machine that runs it. */
typedef struct Session {
    PlumModules modules; /* every entry and import; their arena holds the syntax trees */
    PlumReplScope scope; /* the names entries declared, for the next ones */
    PlumReplCompiler *compiler;
    PlumVM *vm;
    bool exited; /* exit() was called */
    int exit_code;
} Session;

typedef struct Repl {
    Session session;
    PlumRunOptions options;
    bool prompts; /* standard input is a terminal: show prompts */
    bool editing; /* ... and so is standard output: edit lines */
    PlumLineEditor editor;
    bool color;
} Repl;

static void session_init(Session *s, const PlumRunOptions *options) {
    memset(s, 0, sizeof *s);
    plum_modules_init(&s->modules);
    plum_repl_scope_init(&s->scope);
    s->compiler = plum_repl_compiler_new(&s->modules.names);
    s->vm = plum_vm_new(options, stdout, stderr);
}

static void session_free(Session *s) {
    /* The VM first: the values in its globals point at trees and methods
     * the compiler holds. */
    plum_vm_free(s->vm);
    plum_repl_compiler_free(s->compiler);
    plum_repl_scope_free(&s->scope);
    plum_modules_free(&s->modules);
}

/* The expression an entry consists of, if it is one expression. */
static PlumExpr *lone_expr(const PlumModule *entry) {
    const PlumProgram *p = &entry->program;
    if (p->count == 1 && p->stmts[0]->kind == PLUM_STMT_EXPR) {
        return p->stmts[0]->as.expr;
    }
    return NULL;
}

/*
 * An entry's own variables are there for the entries after it, so the
 * flow check's "never used" (W0502) doesn't apply to them; only to the
 * locals of its functions.
 */
static void drop_unused_warnings(PlumModule *entry) {
    PlumDiagList *diags = &entry->diags;
    size_t kept = 0;
    for (size_t i = 0; i < diags->items.len; i++) {
        PlumDiag *d = &diags->items.items[i];
        bool top_level = false;
        for (size_t k = 0; k < entry->program.count && strcmp(d->code, "W0502") == 0; k++) {
            const PlumStmt *s = entry->program.stmts[k];
            top_level =
                top_level || (s->kind == PLUM_STMT_DECL && s->span.offset == d->span.offset);
        }
        if (top_level) {
            plum_free(d->message);
        } else {
            diags->items.items[kept++] = *d;
        }
    }
    diags->items.len = kept;
}

/* Check what an entry brought: the new imports, then the entry itself,
 * which sees the names of the entries before it. */
static bool check_entry(Session *s, PlumModule *entry, PlumModule **added, size_t count,
                        PlumReplScope *names) {
    for (size_t i = 0; i < count; i++) {
        if (added[i]->diags.errors > 0) {
            return false; /* it doesn't lex or parse */
        }
    }
    for (size_t i = 0; i < count; i++) {
        PlumModule *m = added[i];
        if (m == entry) {
            plum_resolve_entry(&m->program, &s->modules.arena, &s->modules.names, &m->diags,
                               &s->scope, names);
        } else {
            plum_resolve(&m->program, &s->modules.arena, &s->modules.names, &m->diags, &m->exports);
        }
        if (m->diags.errors == 0 && m == entry) {
            plum_typecheck_entry(&m->program, &s->modules.arena, &s->modules.names, &m->diags);
        } else if (m->diags.errors == 0) {
            plum_typecheck(&m->program, &s->modules.arena, &s->modules.names, &m->diags);
        }
        if (m->diags.errors == 0) {
            plum_check_flow(&m->program, &m->diags);
            if (m == entry) {
                drop_unused_warnings(m);
            }
        }
        if (m->diags.errors > 0) {
            return false;
        }
    }
    return true;
}

static void print_diags(Repl *r, PlumModule **added, size_t count) {
    fflush(stdout);
    for (size_t i = 0; i < count; i++) {
        plum_diag_print(&added[i]->diags, stderr, r->color);
    }
    fflush(stderr);
}

/*
 * Run one entry: `text` (malloc'd; it moves here) as a module called
 * `path`. With `type_only`, print the type of the expression it is,
 * without running it. An entry that fails leaves nothing declared.
 */
static void run_entry(Repl *r, const char *path, char *text, size_t len, bool type_only) {
    Session *s = &r->session;
    PlumModule **added = NULL;
    size_t count = 0;
    PlumModule *entry = plum_modules_add(&s->modules, path, text, len, &added, &count);
    PlumReplScope names;
    plum_repl_scope_init(&names);
    bool ok = check_entry(s, entry, added, count, &names);

    if (type_only) {
        print_diags(r, added, count);
        PlumExpr *e = lone_expr(entry);
        if (ok && e == NULL) {
            fputs("error: :type needs an expression, like :type len(\"abc\")\n", stderr);
        } else if (ok) {
            char buf[256];
            printf("%s\n", plum_ty_text(e->ty, buf, sizeof buf));
        }
        ok = false; /* nothing ran: what it declared (in a lambda) goes */
    } else {
        PlumCompiled compiled;
        memset(&compiled, 0, sizeof compiled);
        if (ok) {
            PlumCompileModule *list = plum_alloc_zeroed(count, sizeof *list);
            for (size_t i = 0; i < count; i++) {
                list[i].program = &added[i]->program;
                list[i].diags = &added[i]->diags;
                list[i].path = added[i] != entry ? added[i]->path : NULL;
            }
            ok = plum_repl_compile(s->compiler, list, count, true, &compiled);
            plum_free(list);
        }
        /* Warnings and errors come before anything the entry prints. */
        print_diags(r, added, count);
        if (ok) {
            int code = plum_vm_run_entry(s->vm, &compiled, &entry->source);
            plum_release(plum_obj(&compiled.main->obj));
            fflush(stdout);
            fflush(stderr);
            if (plum_vm_exited(s->vm)) {
                s->exited = true;
                s->exit_code = code;
            }
            ok = code == 0;
        }
    }
    if (ok) {
        plum_repl_scope_merge(&s->scope, &names);
    } else {
        plum_modules_forget(&s->modules, added, count);
    }
    plum_repl_scope_free(&names);
    plum_free(added);
}

/* ------------------------------------------------------------------ */
/* When an entry goes on to the next line                              */
/* ------------------------------------------------------------------ */

/* After these a statement can't end, so the entry goes on. */
static bool continues(PlumTokenKind kind) {
    switch (kind) {
    case PLUM_TOK_COMMA:
    case PLUM_TOK_DOT:
    case PLUM_TOK_QUESTION_DOT:
    case PLUM_TOK_COLON:
    case PLUM_TOK_ARROW:
    case PLUM_TOK_FAT_ARROW:
    case PLUM_TOK_DOT_DOT:
    case PLUM_TOK_DOT_DOT_EQ:
    case PLUM_TOK_QUESTION_QUESTION:
    case PLUM_TOK_PLUS:
    case PLUM_TOK_MINUS:
    case PLUM_TOK_STAR:
    case PLUM_TOK_SLASH:
    case PLUM_TOK_SLASH_SLASH:
    case PLUM_TOK_PERCENT:
    case PLUM_TOK_STAR_STAR:
    case PLUM_TOK_AMP:
    case PLUM_TOK_PIPE:
    case PLUM_TOK_CARET:
    case PLUM_TOK_TILDE:
    case PLUM_TOK_LESS_LESS:
    case PLUM_TOK_GREATER_GREATER:
    case PLUM_TOK_EQ_EQ:
    case PLUM_TOK_BANG_EQ:
    case PLUM_TOK_LESS:
    case PLUM_TOK_LESS_EQ:
    case PLUM_TOK_GREATER:
    case PLUM_TOK_GREATER_EQ:
    case PLUM_TOK_AND:
    case PLUM_TOK_OR:
    case PLUM_TOK_NOT:
    case PLUM_TOK_IN:
    case PLUM_TOK_IS:
    case PLUM_TOK_AS:
    case PLUM_TOK_EQ:
    case PLUM_TOK_PLUS_EQ:
    case PLUM_TOK_MINUS_EQ:
    case PLUM_TOK_STAR_EQ:
    case PLUM_TOK_SLASH_EQ:
    case PLUM_TOK_SLASH_SLASH_EQ:
    case PLUM_TOK_PERCENT_EQ:
    case PLUM_TOK_STAR_STAR_EQ:
    case PLUM_TOK_AMP_EQ:
    case PLUM_TOK_PIPE_EQ:
    case PLUM_TOK_CARET_EQ:
    case PLUM_TOK_LESS_LESS_EQ:
    case PLUM_TOK_GREATER_GREATER_EQ:
        return true;
    default:
        return false;
    }
}

/*
 * Does the entry so far need more lines: a bracket still open, a last
 * line ending with an operator, `,` or `=>`, or a """ string not closed?
 */
static bool incomplete(const char *text, size_t len) {
    PlumSource source = {ENTRY_PATH, text, len};
    PlumArena arena;
    PlumInterner names;
    PlumDiagList diags;
    PlumTokenList tokens = {0};
    plum_arena_init(&arena);
    plum_interner_init(&names);
    plum_diag_init(&diags, &source);
    plum_lex(&source, &arena, &names, &diags, &tokens);
    long depth = 0;
    PlumTokenKind last = PLUM_TOK_NEWLINE;
    for (size_t i = 0; i < tokens.len; i++) {
        PlumTokenKind k = tokens.items[i].kind;
        if (k == PLUM_TOK_LPAREN || k == PLUM_TOK_LBRACKET || k == PLUM_TOK_LBRACE) {
            depth++;
        } else if (k == PLUM_TOK_RPAREN || k == PLUM_TOK_RBRACKET || k == PLUM_TOK_RBRACE) {
            depth--;
        }
        if (k != PLUM_TOK_NEWLINE && k != PLUM_TOK_EOF) {
            last = k;
        }
    }
    bool more = depth > 0 || continues(last);
    for (size_t i = 0; i < diags.items.len; i++) {
        const PlumDiag *d = &diags.items.items[i];
        if (strcmp(d->code, "E0120") == 0 && strstr(d->message, "at its end") != NULL) {
            more = true; /* a """ string goes on */
        }
    }
    plum_array_free(&tokens);
    plum_diag_free(&diags);
    plum_interner_free(&names);
    plum_arena_free(&arena);
    return more;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void print_help(void) {
    fputs("Type Plumlang and press Enter: each entry runs at once, and what it declares\n"
          "stays for the entries after it. An entry that is one expression prints its\n"
          "value and type. An entry goes on to the next line while a bracket is open or\n"
          "a line ends with an operator, ',' or '=>'.\n"
          "\n"
          "Commands:\n"
          "  :help           this help\n"
          "  :type <expr>    the type of an expression, without running it\n"
          "  :load <file>    run a file's code in this session\n"
          "  :reset          forget everything declared so far\n"
          "  :quit           leave (also :q, Ctrl-D or exit())\n"
          "\n"
          "Keys: left/right, Home/End (Ctrl-A/E), Backspace/Delete, up/down for earlier\n"
          "lines, Ctrl-U/Ctrl-K delete to the start/end, Ctrl-W the word before the\n"
          "cursor, Ctrl-L clears the screen, Ctrl-C cancels the entry.\n",
          stdout);
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void load_file(Repl *r, const char *path) {
    size_t len = 0;
    int err = 0;
    char *text = plum_read_file(path, &len, &err);
    if (text == NULL) {
        fprintf(stderr, "error: cannot read '%s': %s\n", path, strerror(err));
        return;
    }
    run_entry(r, path, text, len, false);
}

/* A line starting with ':'. Returns false to leave the REPL. */
static bool command(Repl *r, const char *line) {
    const char *p = line + 1;
    const char *name = p;
    while (*p != '\0' && !is_space(*p)) {
        p++;
    }
    size_t n = (size_t)(p - name);
    while (is_space(*p)) {
        p++;
    }
    const char *rest = p;
    size_t rest_len = strlen(rest);
    while (rest_len > 0 && is_space(rest[rest_len - 1])) {
        rest_len--;
    }
#define IS(word) (n == strlen(word) && strncmp(name, word, n) == 0)
    if (IS("help") || IS("h") || IS("?")) {
        print_help();
    } else if (IS("quit") || IS("q") || IS("exit")) {
        return false;
    } else if (IS("type") || IS("t")) {
        if (rest_len == 0) {
            fputs("error: :type needs an expression, like :type len(\"abc\")\n", stderr);
        } else {
            char *text = malloc(rest_len + 2);
            if (text == NULL) {
                plum_fatal("out of memory");
            }
            memcpy(text, rest, rest_len);
            text[rest_len] = '\n';
            text[rest_len + 1] = '\0';
            run_entry(r, ENTRY_PATH, text, rest_len + 1, true);
        }
    } else if (IS("load") || IS("l")) {
        if (rest_len == 0) {
            fputs("error: :load needs a file, like :load shapes.plum\n", stderr);
        } else {
            char *path = plum_strndup(rest, rest_len);
            load_file(r, path);
            plum_free(path);
        }
    } else if (IS("reset")) {
        session_free(&r->session);
        session_init(&r->session, &r->options);
        puts("(everything declared so far is forgotten)");
    } else {
        fprintf(stderr, "error: there is no command ':%.*s'; :help lists them\n", (int)n, name);
    }
#undef IS
    return !r->session.exited;
}

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

static char *copy_text(const Text *t) {
    char *text = malloc(t->len + 1);
    if (text == NULL) {
        plum_fatal("out of memory");
    }
    memcpy(text, t->items, t->len);
    text[t->len] = '\0';
    return text;
}

int plum_repl_main(void) {
    Repl r;
    memset(&r, 0, sizeof r);
    r.prompts = plum_line_input_is_terminal();
    r.editing = r.prompts && plum_line_is_terminal();
    r.color = plum_diag_use_color(stderr);
    session_init(&r.session, &r.options);
    if (r.editing) {
        char *history = plum_line_history_path();
        plum_line_init(&r.editor, history);
        plum_free(history);
    }
    if (r.prompts) {
        printf("Plumlang " PLUM_VERSION ". Type :help for help, :quit to leave.\n");
    }

    Text entry = {0};
    for (;;) {
        const char *prompt = !r.prompts ? "" : entry.len == 0 ? "plum> " : "  ... ";
        char *line = NULL;
        PlumLineStatus status = r.editing ? plum_line_read(&r.editor, prompt, &line)
                                          : plum_line_read_plain(prompt, &line);
        if (status == PLUM_LINE_INTERRUPT) {
            entry.len = 0; /* Ctrl-C: the entry is dropped */
            continue;
        }
        if (status == PLUM_LINE_EOF) {
            if (entry.len > 0) {
                run_entry(&r, ENTRY_PATH, copy_text(&entry), entry.len, false);
            }
            break;
        }
        if (r.editing) {
            plum_line_add_history(&r.editor, line);
        }
        const char *start = line;
        while (is_space(*start)) {
            start++;
        }
        if (entry.len == 0 && *start == '\0') {
            plum_free(line);
            continue;
        }
        if (entry.len == 0 && *start == ':') {
            bool go_on = command(&r, start);
            plum_free(line);
            if (!go_on) {
                break;
            }
            continue;
        }
        for (const char *c = line; *c != '\0'; c++) {
            plum_array_push(&entry, *c);
        }
        plum_array_push(&entry, '\n');
        plum_array_push(&entry, '\0'); /* the lexer wants the text NUL-terminated */
        entry.len--;
        plum_free(line);
        if (incomplete(entry.items, entry.len)) {
            continue;
        }
        run_entry(&r, ENTRY_PATH, copy_text(&entry), entry.len, false);
        entry.len = 0;
        if (r.session.exited) {
            break;
        }
    }
    int code = r.session.exited ? r.session.exit_code : 0;
    plum_array_free(&entry);
    session_free(&r.session);
    if (r.editing) {
        plum_line_free(&r.editor);
    }
    return code;
}
