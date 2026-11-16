/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The language server's JSON, positions and file URIs. */
#include "check.h"
#include "lsp/json.h"
#include "lsp/text.h"
#include "suites.h"
#include "util/alloc.h"
#include "util/arena.h"

#include <string.h>

static bool parses(const char *text) {
    PlumArena arena;
    plum_arena_init(&arena);
    bool ok = plum_json_parse(&arena, text, strlen(text)) != NULL;
    plum_arena_free(&arena);
    return ok;
}

/* Parse and write back. */
static void round_trip(const char *text, const char *expect) {
    PlumArena arena;
    plum_arena_init(&arena);
    PlumJson *v = plum_json_parse(&arena, text, strlen(text));
    CHECK(v != NULL);
    PlumJsonWriter w;
    plum_jw_init(&w);
    plum_jw_value(&w, v);
    size_t len = 0;
    CHECK_STR(plum_jw_text(&w, &len), expect);
    plum_jw_free(&w);
    plum_arena_free(&arena);
}

static void test_json(void) {
    round_trip(" {\"a\" : [1, 2.5, true, false, null], \"b\": {}} ",
               "{\"a\":[1,2.5,true,false,null],\"b\":{}}");
    round_trip("\"tab\\there \\u00e9 \\ud83c\\udf3f \\ud800\"",
               "\"tab\\there \xC3\xA9 \xF0\x9F\x8C\xBF \xEF\xBF\xBD\"");
    round_trip("-0.5e2", "-50");
    round_trip("[]", "[]");

    CHECK(!parses(""));
    CHECK(!parses("{"));
    CHECK(!parses("[1,]"));
    CHECK(!parses("{\"a\" 1}"));
    CHECK(!parses("01"));
    CHECK(!parses("1."));
    CHECK(!parses("\"raw\nnewline\""));
    CHECK(!parses("\"\\x\""));
    CHECK(!parses("tru"));
    CHECK(!parses("1 2"));

    /* Nesting is limited, so deep input can't exhaust the stack. */
    char deep[1001];
    memset(deep, '[', 1000);
    deep[1000] = '\0';
    CHECK(!parses(deep));

    PlumArena arena;
    plum_arena_init(&arena);
    const char *msg = "{\"id\":7,\"params\":{\"uri\":\"file:///x\",\"n\":3}}";
    PlumJson *v = plum_json_parse(&arena, msg, strlen(msg));
    CHECK(plum_json_int(v, "id", -1) == 7);
    CHECK(plum_json_int(v, "missing", -1) == -1);
    CHECK_STR(plum_json_str(plum_json_get(v, "params"), "uri", NULL), "file:///x");
    CHECK(plum_json_str(v, "id", NULL) == NULL);
    plum_arena_free(&arena);

    /* The writer escapes control characters and replaces invalid UTF-8. */
    PlumJsonWriter w;
    plum_jw_init(&w);
    plum_jw_object(&w);
    plum_jw_key(&w, "s");
    plum_jw_stringn(&w, "a\"\\\x01\xFF", 5);
    plum_jw_key(&w, "n");
    plum_jw_int(&w, -3);
    plum_jw_object_end(&w);
    size_t len = 0;
    CHECK_STR(plum_jw_text(&w, &len), "{\"s\":\"a\\\"\\\\\\u0001\xEF\xBF\xBD\",\"n\":-3}");
    plum_jw_free(&w);
}

static void test_positions(void) {
    /* "é" is one UTF-16 unit, the emoji two */
    const char *text = "ab\r\n\xC3\xA9x\xF0\x9F\x8C\xBFy\nlast";
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, text, strlen(text));
    CHECK(lines.count == 3);
    CHECK(plum_lsp_offset(&lines, 0, 1) == 1);
    CHECK(plum_lsp_offset(&lines, 0, 99) == 2); /* clamped before the \r\n */
    CHECK(plum_lsp_offset(&lines, 1, 1) == 6);
    CHECK(plum_lsp_offset(&lines, 1, 4) == 11);
    CHECK(plum_lsp_offset(&lines, 9, 0) == strlen(text));
    long long line, ch;
    plum_lsp_position(&lines, 11, &line, &ch);
    CHECK(line == 1 && ch == 4);
    plum_lsp_position(&lines, 3, &line, &ch); /* between \r and \n */
    CHECK(line == 0 && ch == 2);
    plum_lsp_position(&lines, 1000, &line, &ch);
    CHECK(line == 2 && ch == 4);
    plum_lsp_lines_free(&lines);
}

static void test_uris(void) {
    const char *uri = "file:///home/a%20b/x.plum";
    char *path = plum_lsp_uri_to_path(uri, strlen(uri));
    CHECK_STR(path, "/home/a b/x.plum");
    char *back = plum_lsp_path_to_uri(path);
    CHECK_STR(back, uri);
    plum_free(path);
    plum_free(back);

    const char *win = "file:///c%3A/Users/x.plum";
    path = plum_lsp_uri_to_path(win, strlen(win));
    CHECK_STR(path, "c:/Users/x.plum");
    back = plum_lsp_path_to_uri(path);
    CHECK_STR(back, "file:///c:/Users/x.plum");
    plum_free(path);
    plum_free(back);

    const char *untitled = "untitled:Untitled-1";
    path = plum_lsp_uri_to_path(untitled, strlen(untitled));
    CHECK_STR(path, untitled);
    plum_free(path);
}

void plum_test_lsp(void) {
    test_json();
    test_positions();
    test_uris();
}
