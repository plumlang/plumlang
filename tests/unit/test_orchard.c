/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* The TOML subset, versions, ranges, names and package paths (src/orchard). */
#include "check.h"
#include "orchard/manifest.h"
#include "orchard/toml.h"
#include "suites.h"

#include <string.h>

/* Parse `text`; returns whether it parsed, and the first error's code and line. */
static bool parse(const char *text, PlumToml *doc, const char **code, unsigned *line) {
    PlumSource src = {"t.toml", text, strlen(text)};
    PlumDiagList diags;
    plum_diag_init(&diags, &src);
    bool ok = plum_toml_parse(doc, &src, &diags);
    *code = diags.items.len > 0 ? diags.items.items[0].code : "";
    *line = diags.items.len > 0 ? diags.items.items[0].span.line : 0;
    plum_diag_free(&diags);
    return ok;
}

static bool bad(const char *text, const char *want_code, unsigned want_line) {
    PlumToml doc;
    const char *code;
    unsigned line;
    bool ok = parse(text, &doc, &code, &line);
    plum_toml_free(&doc);
    return !ok && strcmp(code, want_code) == 0 && line == want_line;
}

static void test_toml(void) {
    PlumToml doc;
    const char *code;
    unsigned line;
    const char *text = "\xEF\xBB\xBF# a manifest\r\n"
                       "[package]\n"
                       "name = \"weather\"   # trailing comment\n"
                       "'quoted key' = 'C:\\path'\n"
                       "esc = \"tab\\tquote\\\" \\u00e9 \\U0001F351\"\n"
                       "authors = [\n"
                       "  \"Ada <ada@example.com>\",  # first\n"
                       "  'Grace',\n"
                       "]\n"
                       "empty = []\n"
                       "\n"
                       "[packages.hello-orchard]\n"
                       "version = \"1.0.0\"\n";
    REQUIRE(parse(text, &doc, &code, &line));
    const PlumTomlValue *v = plum_toml_get(&doc, "package", "name");
    REQUIRE(v != NULL && v->kind == PLUM_TOML_STRING);
    CHECK_STR(v->str, "weather");
    CHECK(v->key_span.line == 3 && v->key_span.column == 1);
    CHECK(v->value_span.column == 8);
    v = plum_toml_get(&doc, "package", "quoted key");
    REQUIRE(v != NULL);
    CHECK_STR(v->str, "C:\\path");
    v = plum_toml_get(&doc, "package", "esc");
    REQUIRE(v != NULL);
    CHECK_STR(v->str, "tab\tquote\" \xC3\xA9 \xF0\x9F\x8D\x91");
    v = plum_toml_get(&doc, "package", "authors");
    REQUIRE(v != NULL && v->kind == PLUM_TOML_ARRAY && v->count == 2);
    CHECK_STR(v->items[0], "Ada <ada@example.com>");
    CHECK_STR(v->items[1], "Grace");
    CHECK(v->last_line == 9);
    v = plum_toml_get(&doc, "package", "empty");
    CHECK(v != NULL && v->count == 0);
    CHECK(plum_toml_get(&doc, "packages.hello-orchard", "version") != NULL);
    const PlumTomlTable *t = plum_toml_table(&doc, "package");
    CHECK(t != NULL && t->span.line == 2 && t->last_line == 10);
    CHECK(plum_toml_table(&doc, "missing") == NULL);
    plum_toml_free(&doc);

    REQUIRE(parse("", &doc, &code, &line));
    CHECK(doc.values.len == 0);
    plum_toml_free(&doc);

    CHECK(bad("a = 1\n", "E0901", 1));
    CHECK(bad("a = true\n", "E0901", 1));
    CHECK(bad("\n\na = \"open\n", "E0901", 3));
    CHECK(bad("a = \"x\" b\n", "E0901", 1));
    CHECK(bad("a.b = \"x\"\n", "E0901", 1));
    CHECK(bad("a = { b = \"c\" }\n", "E0901", 1));
    CHECK(bad("[[package]]\n", "E0901", 1));
    CHECK(bad("[package\n", "E0901", 1));
    CHECK(bad("a = \"\"\"x\"\"\"\n", "E0901", 1));
    CHECK(bad("a = \"\\q\"\n", "E0901", 1));
    CHECK(bad("a = \"\\u12\"\n", "E0901", 1));
    CHECK(bad("a = \"\\u0000\"\n", "E0901", 1));
    CHECK(bad("a = \"\\uD800\"\n", "E0901", 1));
    CHECK(bad("a = [\"x\", 1]\n", "E0901", 1));
    CHECK(bad("a = [\"x\"\n", "E0901", 2));
    CHECK(bad("a =\n", "E0901", 1));
    CHECK(bad("= \"x\"\n", "E0901", 1));
    CHECK(bad("a = \"tab\x01\"\n", "E0901", 1));
    CHECK(bad("a = \"\xC3\x28\"\n", "E0901", 1));
    CHECK(bad("a = \"x\"\na = \"y\"\n", "E0902", 2));
    CHECK(bad("[t]\n[u]\n[t]\n", "E0902", 3));
    CHECK(bad("[t]\nu = \"x\"\n[t.u]\n", "E0902", 3));
    CHECK(bad("[t.u]\n[t]\nu = \"x\"\n", "E0902", 3));
}

static PlumVersion v3(uint32_t a, uint32_t b, uint32_t c) {
    PlumVersion v = {a, b, c};
    return v;
}

static bool allows(const char *range, const char *ver) {
    PlumRange r;
    PlumVersion v;
    return plum_range_parse(range, &r) && plum_version_parse(ver, &v) && plum_range_allows(&r, v);
}

static void test_versions(void) {
    PlumVersion v;
    CHECK(plum_version_parse("1.2.3", &v) && v.major == 1 && v.minor == 2 && v.patch == 3);
    CHECK(plum_version_parse("0.0.0", &v));
    CHECK(plum_version_parse("999999999.0.1", &v));
    CHECK(!plum_version_parse("1.2", &v));
    CHECK(!plum_version_parse("01.2.3", &v));
    CHECK(!plum_version_parse("1.2.3-beta", &v));
    CHECK(!plum_version_parse("1.2.3.4", &v));
    CHECK(!plum_version_parse("1000000000.0.0", &v));
    CHECK(!plum_version_parse("", &v));
    CHECK(!plum_version_parse("v1.0.0", &v));
    CHECK(plum_version_compare(v3(1, 2, 3), v3(1, 10, 0)) < 0);
    CHECK(plum_version_compare(v3(2, 0, 0), v3(1, 99, 99)) > 0);
    CHECK(plum_version_compare(v3(1, 2, 3), v3(1, 2, 3)) == 0);

    CHECK(allows("^1.2", "1.2.0") && allows("^1.2", "1.9.9") && !allows("^1.2", "2.0.0"));
    CHECK(!allows("^1.2", "1.1.9"));
    CHECK(allows("1.2", "1.5.0") && !allows("1.2", "2.0.0")); /* bare is ^ */
    CHECK(allows("^0.2.3", "0.2.9") && !allows("^0.2.3", "0.3.0") && !allows("^0.2.3", "0.2.2"));
    CHECK(allows("^0.0.3", "0.0.3") && !allows("^0.0.3", "0.0.4"));
    CHECK(allows("^0", "0.9.0") && !allows("^0", "1.0.0"));
    CHECK(allows("^0.0", "0.0.7") && !allows("^0.0", "0.1.0"));
    CHECK(allows("~1.2.3", "1.2.9") && !allows("~1.2.3", "1.3.0") && !allows("~1.2.3", "1.2.2"));
    CHECK(allows("~1", "1.9.0") && !allows("~1", "2.0.0"));
    CHECK(allows("=1.2.3", "1.2.3") && !allows("=1.2.3", "1.2.4"));
    CHECK(allows(">=2.1", "2.1.0") && allows(">=2.1", "99.0.0") && !allows(">=2.1", "2.0.9"));
    CHECK(allows("*", "0.0.1") && allows("*", "123.4.5"));
    PlumRange r;
    CHECK(!plum_range_parse("", &r));
    CHECK(!plum_range_parse("^", &r));
    CHECK(!plum_range_parse("<2.0", &r));
    CHECK(!plum_range_parse(">=1.0, <2.0", &r));
    CHECK(!plum_range_parse("^1.x", &r));
    CHECK(!plum_range_parse("latest", &r));
}

static void test_names(void) {
    char why[160];
    CHECK(plum_package_name_valid("weather", why, sizeof why));
    CHECK(plum_package_name_valid("hello-orchard", why, sizeof why));
    CHECK(plum_package_name_valid("a1", why, sizeof why));
    CHECK(!plum_package_name_valid("a", why, sizeof why));
    CHECK(!plum_package_name_valid("Weather", why, sizeof why));
    CHECK(!plum_package_name_valid("1weather", why, sizeof why));
    CHECK(!plum_package_name_valid("my_pkg", why, sizeof why));
    CHECK(!plum_package_name_valid("my--pkg", why, sizeof why));
    CHECK(!plum_package_name_valid("pkg-", why, sizeof why));
    CHECK(!plum_package_name_valid("a2345678901234567890123456789012345678901", why, sizeof why));
    CHECK(plum_package_name_valid("a23456789012345678901234567890123456789", why, sizeof why));
    CHECK(!plum_package_name_valid("json", why, sizeof why));
    CHECK(!plum_package_name_valid("math", why, sizeof why));
    CHECK(!plum_package_name_valid("net", why, sizeof why));
    CHECK(!plum_package_name_valid("hash", why, sizeof why));
    CHECK(!plum_package_name_valid("task", why, sizeof why)); /* task.methods */
    CHECK(!plum_package_name_valid("plum", why, sizeof why));
    CHECK(!plum_package_name_valid("tree", why, sizeof why));
    CHECK(!plum_package_name_valid("true", why, sizeof why));
    CHECK(plum_package_name_valid("trees", why, sizeof why));
    CHECK(plum_package_name_valid("jsonx", why, sizeof why));
    char buf[16];
    CHECK(plum_package_from_seed("hello_orchard", buf, sizeof buf));
    CHECK_STR(buf, "hello-orchard");
    CHECK(!plum_package_from_seed("a_very_long_seed_name", buf, sizeof buf));
}

static void test_paths(void) {
    CHECK(plum_package_path_problem("lib.plum") == NULL);
    CHECK(plum_package_path_problem("src/deep/mod_1.plum") == NULL);
    CHECK(plum_package_path_problem("plum.toml") == NULL);
    CHECK(plum_package_path_problem("README.md") == NULL);
    CHECK(plum_package_path_problem("LICENSE") == NULL);
    CHECK(plum_package_path_problem("LICENSE-MIT.txt") == NULL);
    CHECK(plum_package_path_problem("") != NULL);
    CHECK(plum_package_path_problem("../evil.plum") != NULL);
    CHECK(plum_package_path_problem("a/../../evil.plum") != NULL);
    CHECK(plum_package_path_problem("./a.plum") != NULL);
    CHECK(plum_package_path_problem("/etc/passwd.plum") != NULL);
    CHECK(plum_package_path_problem("\\evil.plum") != NULL);
    CHECK(plum_package_path_problem("C:/evil.plum") != NULL);
    CHECK(plum_package_path_problem("c:evil.plum") != NULL);
    CHECK(plum_package_path_problem("a\\b.plum") != NULL);
    CHECK(plum_package_path_problem("a//b.plum") != NULL);
    CHECK(plum_package_path_problem("a/") != NULL);
    CHECK(plum_package_path_problem(".hidden.plum") != NULL);
    CHECK(plum_package_path_problem("a/.git/x.plum") != NULL);
    CHECK(plum_package_path_problem("con.plum") != NULL);
    CHECK(plum_package_path_problem("sub/NUL.plum") != NULL);
    CHECK(plum_package_path_problem("lpt1.plum") != NULL);
    CHECK(plum_package_path_problem("console.plum") == NULL);
    CHECK(plum_package_path_problem("trailing.") != NULL);
    CHECK(plum_package_path_problem("script.sh") != NULL);
    CHECK(plum_package_path_problem("sub/plum.toml") != NULL);
    CHECK(plum_package_path_problem("sub/README.md") != NULL);
    CHECK(plum_package_path_problem(".plum") != NULL);
    CHECK(plum_package_path_problem("a b.plum") != NULL);
    CHECK(plum_package_path_problem("a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q.plum") != NULL);
}

static void test_utf8(void) {
    CHECK(plum_utf8_valid("plain", 5));
    CHECK(plum_utf8_valid("\xC3\xA9\xF0\x9F\x8D\x91", 6));
    CHECK(!plum_utf8_valid("a\0b", 3));
    CHECK(!plum_utf8_valid("\xC0\xAF", 2));         /* overlong */
    CHECK(!plum_utf8_valid("\xED\xA0\x80", 3));     /* a surrogate */
    CHECK(!plum_utf8_valid("\xF4\x90\x80\x80", 4)); /* past U+10FFFF */
    CHECK(!plum_utf8_valid("\xE2\x82", 2));         /* cut short */
    CHECK(!plum_utf8_valid("\xFF", 1));
}

void plum_test_orchard(void) {
    test_toml();
    test_versions();
    test_names();
    test_paths();
    test_utf8();
}
