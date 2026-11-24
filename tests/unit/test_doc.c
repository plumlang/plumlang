/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* Doc comments (spec 2.2): which `##` lines document what. */
#include "check.h"
#include "parser/doc.h"
#include "parser/parser.h"
#include "suites.h"
#include "util/alloc.h"

#include <string.h>

typedef struct Parsed {
    PlumSource source;
    PlumArena arena;
    PlumInterner names;
    PlumDiagList diags;
    PlumTokenList tokens;
    PlumCommentList comments;
    PlumProgram program;
} Parsed;

static void parse(Parsed *p, const char *text) {
    memset(p, 0, sizeof *p);
    p->source.path = "doc.plum";
    p->source.text = text;
    p->source.length = strlen(text);
    plum_arena_init(&p->arena);
    plum_interner_init(&p->names);
    plum_diag_init(&p->diags, &p->source);
    plum_lex_comments(&p->source, &p->arena, &p->names, &p->diags, &p->tokens, &p->comments);
    PlumParser parser;
    plum_parser_init(&parser, &p->tokens, &p->arena, &p->names, &p->diags);
    p->program = plum_parse_program(&parser);
}

static void done(Parsed *p) {
    plum_array_free(&p->comments);
    plum_array_free(&p->tokens);
    plum_diag_free(&p->diags);
    plum_interner_free(&p->names);
    plum_arena_free(&p->arena);
}

/* Does `got` (freed here) equal `want` (NULL: no doc)? */
static int same(char *got, const char *want) {
    int ok = want == NULL ? got == NULL : got != NULL && strcmp(got, want) == 0;
    if (!ok) {
        fprintf(stderr, "  doc: got [%s], want [%s]\n", got != NULL ? got : "(none)",
                want != NULL ? want : "(none)");
    }
    plum_free(got);
    return ok;
}

/* The doc of the i-th top-level statement, and the module's. */
static void check_doc(const char *text, size_t i, const char *want, const char *module) {
    Parsed p;
    parse(&p, text);
    CHECK(p.diags.errors == 0);
    if (i < p.program.count) {
        const PlumStmt *s = p.program.stmts[i];
        CHECK(same(plum_doc_documents(s) ? plum_doc_before(&p.source, &p.comments, s->span) : NULL,
                   want));
    }
    CHECK(same(plum_doc_module(&p.source, &p.comments, &p.program), module));
    done(&p);
}

void plum_test_doc(void) {
    /* `##` lines right above a declaration; one space after ## is dropped */
    check_doc("seed math\n## Adds.\n##   indented\n##\n##no space\nbranch f() {}\n", 1,
              "Adds.\n  indented\n\nno space", NULL);
    /* a blank line, or an ordinary comment, in between: no doc */
    check_doc("seed math\n## Lost.\n\nbranch f() {}\n", 1, NULL, NULL);
    check_doc("seed math\n## Lost.\n# plain\nbranch f() {}\n", 1, NULL, NULL);
    /* ### is an ordinary comment, so is ## after code */
    check_doc("seed math\n### Section\nroot A = 1\n", 1, NULL, NULL);
    check_doc("seed math\nplant x = 1 ## not a doc\nroot A = 1\n", 1, NULL, NULL);
    /* indented, before a declaration in a block */
    check_doc("seed math\n    ## Indented.\n    root A = 1\n", 1, "Indented.", NULL);
    /* the first lines document the module, unless a declaration follows them */
    check_doc("## The module.\n## More.\n\n## The constant.\nroot A = 1\n", 0, "The constant.",
              "The module.\nMore.");
    check_doc("## Only the constant.\nroot A = 1\n", 0, "Only the constant.", NULL);
    check_doc("# SPDX header\n\n## The module.\nseed math\n", 0, NULL, "The module.");
    check_doc("seed math\n## Too late for the module.\nplant x = 1\n", 1, NULL, NULL);
    check_doc("## Just a module.\n", 0, NULL, "Just a module.");
    /* a `##` line inside a block comment, or a string, is neither */
    check_doc("#[\n## inside\n]#\nroot A = 1\n", 0, NULL, NULL);
    check_doc("root S = \"\"\"\n    ## text\n    \"\"\"\nroot A = 1\n", 1, NULL, NULL);
    /* plants are not documented */
    check_doc("seed math\n## No.\nplant x = 1\n", 1, NULL, NULL);

    /* fields, methods and cases: by their spans */
    Parsed p;
    parse(&p, "tree T {\n    ## The x.\n    x: int\n    ## Says hi.\n    branch hi() {}\n}\n"
              "kind K {\n    ## First.\n    A\n    B\n}\n");
    REQUIRE(p.diags.errors == 0 && p.program.count == 2);
    const PlumStmt *t = p.program.stmts[0];
    const PlumStmt *k = p.program.stmts[1];
    CHECK(same(plum_doc_before(&p.source, &p.comments, t->as.tree.fields[0].span), "The x."));
    CHECK(same(plum_doc_before(&p.source, &p.comments, t->as.tree.methods[0].span), "Says hi."));
    CHECK(same(plum_doc_before(&p.source, &p.comments, k->as.kind.cases[0].span), "First."));
    CHECK(same(plum_doc_before(&p.source, &p.comments, k->as.kind.cases[1].span), NULL));
    done(&p);

    /* comment kinds and where block comments end */
    parse(&p, "#[ a\n #[ b ]#\n]# ## doc\n### line\n");
    REQUIRE(p.comments.len == 3);
    CHECK(p.comments.items[0].kind == PLUM_COMMENT_BLOCK);
    CHECK(p.comments.items[0].span.line == 1 && p.comments.items[0].end_line == 3);
    CHECK(p.comments.items[1].kind == PLUM_COMMENT_DOC && !p.comments.items[1].own_line);
    CHECK(p.comments.items[2].kind == PLUM_COMMENT_LINE);
    done(&p);
}
