/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The language server: reading and writing messages, the documents the
 * editor has open, and which request does what.
 */
#include "lsp/lsp.h"

#include "fmt/format.h"
#include "lsp/analysis.h"
#include "lsp/json.h"
#include "lsp/text.h"
#include "plum/version.h"
#include "stdlib/platform.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/array.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* JSON-RPC and LSP error codes. */
enum {
    ERR_PARSE = -32700,
    ERR_INVALID_REQUEST = -32600,
    ERR_METHOD_NOT_FOUND = -32601,
    ERR_INVALID_PARAMS = -32602,
    ERR_NOT_INITIALIZED = -32002,
    ERR_REQUEST_FAILED = -32803,
};

/* A message bigger than this is read and dropped. */
#define MAX_MESSAGE ((size_t)256 * 1024 * 1024)
#define MAX_HEADER  4096

typedef struct Doc {
    char *uri;
    char *path;
    char *text; /* NUL-terminated */
    size_t len;
    PlumLspAnalysis *analysis;
} Doc;

/* Diagnostics published for a file that isn't open: one an open file
 * imports, which `plum check` of that file would report. */
typedef struct Published {
    char *uri;
    char *json; /* the diagnostics array as sent, to send again only on change */
} Published;

typedef struct Server {
    FILE *in;
    FILE *out;
    PLUM_ARRAY(Doc *) docs;
    PLUM_ARRAY(Published) extra;
    bool initialized;
    bool shutdown;
    bool exited;
} Server;

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

static void send(Server *s, PlumJsonWriter *w) {
    size_t len = 0;
    const char *text = plum_jw_text(w, &len);
    fprintf(s->out, "Content-Length: %zu\r\n\r\n", len);
    fwrite(text, 1, len, s->out);
    fflush(s->out);
}

/* {"jsonrpc":"2.0","id":..., then the caller writes "result" or "error". */
static void response_start(PlumJsonWriter *w, const PlumJson *id) {
    plum_jw_init(w);
    plum_jw_object(w);
    plum_jw_key(w, "jsonrpc");
    plum_jw_string(w, "2.0");
    plum_jw_key(w, "id");
    plum_jw_value(w, id);
}

static void send_error(Server *s, const PlumJson *id, int code, const char *message) {
    PlumJsonWriter w;
    response_start(&w, id);
    plum_jw_key(&w, "error");
    plum_jw_object(&w);
    plum_jw_key(&w, "code");
    plum_jw_int(&w, code);
    plum_jw_key(&w, "message");
    plum_jw_string(&w, message);
    plum_jw_object_end(&w);
    plum_jw_object_end(&w);
    send(s, &w);
    plum_jw_free(&w);
}

static bool name_is(const char *line, size_t n, const char *name) {
    size_t k = strlen(name);
    if (n < k) {
        return false;
    }
    for (size_t i = 0; i < k; i++) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

/* One header line, without its line break; false at the end of input. A
 * line too long is cut (the rest is read and dropped). */
static bool read_line(FILE *in, char *buf, size_t size, size_t *len) {
    size_t n = 0;
    int c;
    bool any = false;
    while ((c = fgetc(in)) != EOF) {
        any = true;
        if (c == '\n') {
            break;
        }
        if (n + 1 < size) {
            buf[n++] = (char)c;
        }
    }
    if (!any) {
        return false;
    }
    if (n > 0 && buf[n - 1] == '\r') {
        n--;
    }
    buf[n] = '\0';
    *len = n;
    return true;
}

/*
 * Read the next message: 1 with its body (malloc'd) in *body, 0 at the
 * end of the input, -1 for a message without a usable Content-Length
 * (dropped).
 */
static int read_message(Server *s, char **body, size_t *len) {
    char line[MAX_HEADER];
    size_t n = 0;
    bool have_length = false;
    bool bad = false;
    unsigned long long length = 0;
    bool any = false;
    for (;;) {
        if (!read_line(s->in, line, sizeof line, &n)) {
            return 0;
        }
        if (n == 0) {
            if (!any) {
                continue; /* stray blank lines between messages */
            }
            break;
        }
        any = true;
        if (name_is(line, n, "content-length:")) {
            const char *p = line + 15;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            char *end = NULL;
            errno = 0;
            length = strtoull(p, &end, 10);
            have_length = end != p && errno == 0 && (*end == '\0' || *end == ' ');
            bad = !have_length;
        }
    }
    if (!have_length || bad) {
        return -1;
    }
    if (length > MAX_MESSAGE) {
        for (unsigned long long i = 0; i < length; i++) {
            if (fgetc(s->in) == EOF) {
                return 0;
            }
        }
        return -1;
    }
    char *buf = plum_alloc((size_t)length + 1);
    size_t got = fread(buf, 1, (size_t)length, s->in);
    buf[got] = '\0';
    if (got < (size_t)length) {
        plum_free(buf);
        return 0;
    }
    *body = buf;
    *len = got;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Documents                                                           */
/* ------------------------------------------------------------------ */

static Doc *doc_by_uri(Server *s, const char *uri) {
    for (size_t i = 0; i < s->docs.len; i++) {
        if (strcmp(s->docs.items[i]->uri, uri) == 0) {
            return s->docs.items[i];
        }
    }
    return NULL;
}

static Doc *doc_by_path(Server *s, const char *path) {
    for (size_t i = 0; i < s->docs.len; i++) {
        if (plum_lsp_same_path(s->docs.items[i]->path, path)) {
            return s->docs.items[i];
        }
    }
    return NULL;
}

/* Imported files: the editor's text when the file is open, else the disk's. */
static char *read_module(void *ctx, const char *path, size_t *len, int *err) {
    Doc *d = doc_by_path(ctx, path);
    if (d == NULL) {
        return plum_read_file(path, len, err);
    }
    char *copy = malloc(d->len + 1); /* the loader frees it with free() */
    if (copy == NULL) {
        *err = ENOMEM;
        return NULL;
    }
    memcpy(copy, d->text, d->len + 1);
    *len = d->len;
    return copy;
}

static char *uri_of(void *ctx, const char *path) {
    if (path[0] == '<') {
        return NULL; /* a standard module built into plum */
    }
    Doc *d = doc_by_path(ctx, path);
    return d != NULL ? plum_strdup(d->uri) : plum_lsp_path_to_uri(path);
}

static void analyze(Server *s, Doc *d) {
    plum_lsp_analysis_free(d->analysis);
    d->analysis = plum_lsp_analyze(d->path, d->text, d->len, read_module, s);
}

static void publish(Server *s, const char *uri, const char *diagnostics) {
    PlumJsonWriter w;
    plum_jw_init(&w);
    plum_jw_object(&w);
    plum_jw_key(&w, "jsonrpc");
    plum_jw_string(&w, "2.0");
    plum_jw_key(&w, "method");
    plum_jw_string(&w, "textDocument/publishDiagnostics");
    plum_jw_key(&w, "params");
    plum_jw_object(&w);
    plum_jw_key(&w, "uri");
    plum_jw_string(&w, uri);
    plum_jw_key(&w, "diagnostics");
    /* already JSON: written as it is */
    size_t n = strlen(diagnostics);
    plum_array_reserve(&w.buf, n);
    memcpy(w.buf.items + w.buf.len, diagnostics, n);
    w.buf.len += n;
    w.after_key = false;
    plum_jw_object_end(&w);
    plum_jw_object_end(&w);
    send(s, &w);
    plum_jw_free(&w);
}

static char *diagnostics_json(const PlumModule *m) {
    PlumJsonWriter w;
    plum_jw_init(&w);
    plum_lsp_write_diagnostics(&w, m);
    size_t len = 0;
    char *copy = plum_strdup(plum_jw_text(&w, &len));
    plum_jw_free(&w);
    return copy;
}

static void publish_doc(Server *s, const Doc *d) {
    char *json = diagnostics_json(d->analysis->main);
    publish(s, d->uri, json);
    plum_free(json);
}

/*
 * Files that open files import, which aren't open themselves: their
 * diagnostics (what `plum check` of the importer reports for them) are
 * published once, and again when they change or go away.
 */
static void publish_imports(Server *s) {
    PLUM_ARRAY(Published) now = {0};
    for (size_t i = 0; i < s->docs.len; i++) {
        const PlumLspAnalysis *a = s->docs.items[i]->analysis;
        for (size_t k = 0; a != NULL && k + 1 < a->count; k++) {
            const PlumModule *m = a->order[k];
            if (m->path[0] == '<' || m->diags.items.len == 0 || doc_by_path(s, m->path) != NULL) {
                continue;
            }
            char *uri = plum_lsp_path_to_uri(m->path);
            bool seen = false;
            for (size_t j = 0; j < now.len && !seen; j++) {
                seen = strcmp(now.items[j].uri, uri) == 0;
            }
            if (seen) {
                plum_free(uri);
                continue;
            }
            Published p = {uri, diagnostics_json(m)};
            plum_array_push(&now, p);
        }
    }
    for (size_t j = 0; j < now.len; j++) {
        bool same = false;
        for (size_t i = 0; i < s->extra.len; i++) {
            if (strcmp(s->extra.items[i].uri, now.items[j].uri) == 0) {
                same = strcmp(s->extra.items[i].json, now.items[j].json) == 0;
                break;
            }
        }
        if (!same) {
            publish(s, now.items[j].uri, now.items[j].json);
        }
    }
    for (size_t i = 0; i < s->extra.len; i++) {
        bool still = false;
        for (size_t j = 0; j < now.len && !still; j++) {
            still = strcmp(s->extra.items[i].uri, now.items[j].uri) == 0;
        }
        if (!still && doc_by_uri(s, s->extra.items[i].uri) == NULL) {
            publish(s, s->extra.items[i].uri, "[]");
        }
        plum_free(s->extra.items[i].uri);
        plum_free(s->extra.items[i].json);
    }
    plum_array_free(&s->extra);
    s->extra.items = now.items;
    s->extra.len = now.len;
    s->extra.cap = now.cap;
}

/* `path` changed (or opened, or closed): check it, and every open file
 * that imports it, again, and publish what they report. */
static void changed(Server *s, const char *path) {
    for (size_t i = 0; i < s->docs.len; i++) {
        Doc *d = s->docs.items[i];
        if (plum_lsp_same_path(d->path, path) ||
            (d->analysis != NULL && plum_lsp_analysis_uses(d->analysis, path))) {
            analyze(s, d);
            publish_doc(s, d);
        }
    }
    publish_imports(s);
}

static void doc_free(Doc *d) {
    plum_lsp_analysis_free(d->analysis);
    plum_free(d->uri);
    plum_free(d->path);
    plum_free(d->text);
    plum_free(d);
}

/* ------------------------------------------------------------------ */
/* The package                                                         */
/* ------------------------------------------------------------------ */

/*
 * References and rename look through the files of the package the file
 * belongs to: every .plum file under the nearest directory up with a
 * plum.toml, or, without one, the .plum files beside it; and every file
 * that is open, or that an open file imports.
 */
#define PACKAGE_FILES 4096
#define PACKAGE_DEPTH 16

typedef PLUM_ARRAY(char *) Paths;

static void add_path(Paths *paths, const char *path) {
    if (path == NULL || path[0] == '<' || paths->len >= PACKAGE_FILES) {
        return;
    }
    for (size_t i = 0; i < paths->len; i++) {
        if (plum_lsp_same_path(paths->items[i], path)) {
            return;
        }
    }
    plum_array_push(paths, plum_strdup(path));
}

static void free_paths(Paths *paths) {
    for (size_t i = 0; i < paths->len; i++) {
        plum_free(paths->items[i]);
    }
    plum_array_free(paths);
}

static void add_entry(void *ctx, const char *name) {
    Paths *names = ctx;
    plum_array_push(names, plum_strdup(name));
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool is_plum_file(const char *name) {
    size_t n = strlen(name);
    return n > 5 && strcmp(name + n - 5, ".plum") == 0;
}

/* The .plum files in `dir` (and below it, with `deep`), in name order;
 * hidden entries are skipped. */
static void add_dir(Paths *paths, const char *dir, bool deep, int depth) {
    Paths names = {0};
    if (depth > PACKAGE_DEPTH || plum_os_list_dir(dir, add_entry, &names) != 0) {
        free_paths(&names);
        return;
    }
    if (names.len > 1) {
        qsort(names.items, names.len, sizeof(char *), by_name);
    }
    for (size_t i = 0; i < names.len; i++) {
        const char *name = names.items[i];
        if (name[0] == '.') {
            continue;
        }
        size_t size = strlen(dir) + strlen(name) + 2;
        char *path = plum_alloc(size);
        snprintf(path, size, "%s/%s", dir, name);
        if (is_plum_file(name)) {
            add_path(paths, path);
        } else if (deep && plum_os_is_dir(path)) {
            add_dir(paths, path, deep, depth + 1);
        }
        plum_free(path);
    }
    free_paths(&names);
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
    }
    return f != NULL;
}

/* The directory of the package `path` is in, and whether it has a
 * plum.toml (malloc'd); NULL for a path with no directory. */
static char *package_dir(const char *path, bool *manifest) {
    *manifest = false;
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
        return NULL;
    }
    char *own = plum_strndup(path, (size_t)(slash - path));
    char *dir = plum_strdup(own);
    for (int up = 0; up < 32; up++) {
        size_t size = strlen(dir) + sizeof "/plum.toml";
        char *toml = plum_alloc(size);
        snprintf(toml, size, "%s/plum.toml", dir);
        bool found = file_exists(toml);
        plum_free(toml);
        if (found) {
            *manifest = true;
            plum_free(own);
            return dir;
        }
        char *parent = strrchr(dir, '/');
        if (parent == NULL || parent == dir) {
            break;
        }
        *parent = '\0';
    }
    plum_free(dir);
    return own;
}

/* The files to look through for `t`, asked about in `d`: `d` first. */
static void package_files(Server *s, const Doc *d, const PlumLspTarget *t, Paths *out) {
    add_path(out, d->path);
    add_path(out, t->path);
    for (size_t i = 0; i < s->docs.len; i++) {
        add_path(out, s->docs.items[i]->path);
    }
    for (size_t i = 0; i < s->docs.len; i++) {
        const PlumLspAnalysis *a = s->docs.items[i]->analysis;
        for (size_t k = 0; a != NULL && k < a->count; k++) {
            add_path(out, a->order[k]->path);
        }
    }
    bool manifest = false;
    char *dir = package_dir(d->path, &manifest);
    if (dir != NULL) {
        add_dir(out, dir, manifest, 0);
    }
    plum_free(dir);
}

/* ------------------------------------------------------------------ */
/* Notifications                                                       */
/* ------------------------------------------------------------------ */

static const char *doc_uri(const PlumJson *params) {
    return plum_json_str(plum_json_get(params, "textDocument"), "uri", NULL);
}

static void did_open(Server *s, const PlumJson *params) {
    const PlumJson *td = plum_json_get(params, "textDocument");
    size_t uri_len = 0, text_len = 0;
    const char *uri = plum_json_str(td, "uri", &uri_len);
    const char *text = plum_json_str(td, "text", &text_len);
    if (uri == NULL || text == NULL) {
        return;
    }
    Doc *d = doc_by_uri(s, uri);
    if (d == NULL) {
        d = plum_alloc_zeroed(1, sizeof *d);
        d->uri = plum_strndup(uri, uri_len);
        d->path = plum_lsp_uri_to_path(uri, uri_len);
        plum_array_push(&s->docs, d);
    } else {
        plum_free(d->text);
    }
    d->text = plum_strndup(text, text_len);
    d->len = text_len;
    changed(s, d->path);
}

/* Replace text[start, end) of a document with `with`. */
static void splice(Doc *d, size_t start, size_t end, const char *with, size_t n) {
    size_t len = d->len - (end - start) + n;
    char *text = plum_alloc(len + 1);
    memcpy(text, d->text, start);
    memcpy(text + start, with, n);
    memcpy(text + start + n, d->text + end, d->len - end);
    text[len] = '\0';
    plum_free(d->text);
    d->text = text;
    d->len = len;
}

static void did_change(Server *s, const PlumJson *params) {
    const char *uri = doc_uri(params);
    Doc *d = uri != NULL ? doc_by_uri(s, uri) : NULL;
    const PlumJson *changes = plum_json_get(params, "contentChanges");
    if (d == NULL || changes == NULL || changes->kind != PLUM_JSON_ARRAY) {
        return;
    }
    for (size_t i = 0; i < changes->as.arr.count; i++) {
        const PlumJson *c = changes->as.arr.items[i];
        size_t n = 0;
        const char *text = plum_json_str(c, "text", &n);
        if (text == NULL) {
            continue;
        }
        const PlumJson *range = plum_json_get(c, "range");
        if (range == NULL) {
            splice(d, 0, d->len, text, n);
            continue;
        }
        /* an incremental change, though the server asks for whole texts */
        PlumLspLines lines;
        plum_lsp_lines_init(&lines, d->text, d->len);
        const PlumJson *from = plum_json_get(range, "start");
        const PlumJson *to = plum_json_get(range, "end");
        size_t start = plum_lsp_offset(&lines, plum_json_int(from, "line", 0),
                                       plum_json_int(from, "character", 0));
        size_t end = plum_lsp_offset(&lines, plum_json_int(to, "line", 0),
                                     plum_json_int(to, "character", 0));
        plum_lsp_lines_free(&lines);
        if (end < start) {
            end = start;
        }
        splice(d, start, end, text, n);
    }
    changed(s, d->path);
}

static void did_close(Server *s, const PlumJson *params) {
    const char *uri = doc_uri(params);
    for (size_t i = 0; uri != NULL && i < s->docs.len; i++) {
        Doc *d = s->docs.items[i];
        if (strcmp(d->uri, uri) != 0) {
            continue;
        }
        memmove(&s->docs.items[i], &s->docs.items[i + 1], (s->docs.len - i - 1) * sizeof(Doc *));
        s->docs.len--;
        publish(s, d->uri, "[]");
        char *path = plum_strdup(d->path);
        doc_free(d);
        changed(s, path); /* files importing it now read it from disk */
        plum_free(path);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

static void initialize_result(PlumJsonWriter *w) {
    plum_jw_object(w);
    plum_jw_key(w, "capabilities");
    plum_jw_object(w);
    plum_jw_key(w, "positionEncoding");
    plum_jw_string(w, "utf-16");
    plum_jw_key(w, "textDocumentSync");
    plum_jw_object(w);
    plum_jw_key(w, "openClose");
    plum_jw_bool(w, true);
    plum_jw_key(w, "change");
    plum_jw_int(w, 1); /* the whole text on every change */
    plum_jw_object_end(w);
    plum_jw_key(w, "hoverProvider");
    plum_jw_bool(w, true);
    plum_jw_key(w, "definitionProvider");
    plum_jw_bool(w, true);
    plum_jw_key(w, "completionProvider");
    plum_jw_object(w);
    plum_jw_key(w, "triggerCharacters");
    plum_jw_array(w);
    plum_jw_string(w, ".");
    plum_jw_array_end(w);
    plum_jw_object_end(w);
    plum_jw_key(w, "documentSymbolProvider");
    plum_jw_bool(w, true);
    plum_jw_key(w, "documentFormattingProvider");
    plum_jw_bool(w, true);
    plum_jw_key(w, "referencesProvider");
    plum_jw_bool(w, true);
    plum_jw_key(w, "renameProvider");
    plum_jw_object(w);
    plum_jw_key(w, "prepareProvider");
    plum_jw_bool(w, true);
    plum_jw_object_end(w);
    plum_jw_key(w, "signatureHelpProvider");
    plum_jw_object(w);
    plum_jw_key(w, "triggerCharacters");
    plum_jw_array(w);
    plum_jw_string(w, "(");
    plum_jw_string(w, ",");
    plum_jw_array_end(w);
    plum_jw_key(w, "retriggerCharacters");
    plum_jw_array(w);
    plum_jw_string(w, ")");
    plum_jw_array_end(w);
    plum_jw_object_end(w);
    plum_jw_key(w, "codeActionProvider");
    plum_jw_object(w);
    plum_jw_key(w, "codeActionKinds");
    plum_jw_array(w);
    plum_jw_string(w, "quickfix");
    plum_jw_array_end(w);
    plum_jw_object_end(w);
    plum_jw_key(w, "semanticTokensProvider");
    plum_jw_object(w);
    plum_jw_key(w, "legend");
    plum_lsp_semantic_legend(w);
    plum_jw_key(w, "full");
    plum_jw_bool(w, true);
    plum_jw_object_end(w);
    plum_jw_object_end(w);
    plum_jw_key(w, "serverInfo");
    plum_jw_object(w);
    plum_jw_key(w, "name");
    plum_jw_string(w, "plum");
    plum_jw_key(w, "version");
    plum_jw_string(w, PLUM_VERSION);
    plum_jw_object_end(w);
    plum_jw_object_end(w);
}

/* The whole text, replaced by its formatted form; [] when it is already
 * formatted, null when it has syntax errors. */
static void formatting(PlumJsonWriter *w, const Doc *d) {
    PlumSource source = {d->path, d->text, d->len};
    PlumDiagList diags;
    plum_diag_init(&diags, &source);
    size_t len = 0;
    char *formatted = plum_format(&source, &diags, &len);
    plum_diag_free(&diags);
    if (formatted == NULL) {
        plum_jw_null(w);
        return;
    }
    plum_jw_array(w);
    if (len != d->len || memcmp(formatted, d->text, len) != 0) {
        PlumLspLines lines;
        plum_lsp_lines_init(&lines, d->text, d->len);
        long long line, character;
        plum_lsp_position(&lines, d->len, &line, &character);
        plum_lsp_lines_free(&lines);
        plum_jw_object(w);
        plum_jw_key(w, "range");
        plum_jw_object(w);
        plum_jw_key(w, "start");
        plum_jw_object(w);
        plum_jw_key(w, "line");
        plum_jw_int(w, 0);
        plum_jw_key(w, "character");
        plum_jw_int(w, 0);
        plum_jw_object_end(w);
        plum_jw_key(w, "end");
        plum_jw_object(w);
        plum_jw_key(w, "line");
        plum_jw_int(w, line);
        plum_jw_key(w, "character");
        plum_jw_int(w, character);
        plum_jw_object_end(w);
        plum_jw_object_end(w);
        plum_jw_key(w, "newText");
        plum_jw_stringn(w, formatted, len);
        plum_jw_object_end(w);
    }
    plum_jw_array_end(w);
    plum_free(formatted);
}

/* The byte offset of a position in a document. */
static size_t offset_of(const Doc *d, const PlumJson *pos) {
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, d->text, d->len);
    size_t offset =
        plum_lsp_offset(&lines, plum_json_int(pos, "line", 0), plum_json_int(pos, "character", 0));
    plum_lsp_lines_free(&lines);
    return offset;
}

static void write_range(PlumJsonWriter *w, const PlumLspLines *lines, size_t start, size_t end) {
    long long line, character;
    plum_jw_object(w);
    plum_jw_key(w, "start");
    plum_lsp_position(lines, start, &line, &character);
    plum_jw_object(w);
    plum_jw_key(w, "line");
    plum_jw_int(w, line);
    plum_jw_key(w, "character");
    plum_jw_int(w, character);
    plum_jw_object_end(w);
    plum_jw_key(w, "end");
    plum_lsp_position(lines, end, &line, &character);
    plum_jw_object(w);
    plum_jw_key(w, "line");
    plum_jw_int(w, line);
    plum_jw_key(w, "character");
    plum_jw_int(w, character);
    plum_jw_object_end(w);
    plum_jw_object_end(w);
}

/*
 * Every place in the package that names `t`: Location[] for references,
 * or, with `new_name`, a WorkspaceEdit that renames them all.
 */
static void references(Server *s, PlumJsonWriter *w, const Doc *d, const PlumLspTarget *t,
                       bool declaration, const char *new_name) {
    Paths paths = {0};
    package_files(s, d, t, &paths);
    if (new_name != NULL) {
        plum_jw_object(w);
        plum_jw_key(w, "changes");
        plum_jw_object(w);
    } else {
        plum_jw_array(w);
    }
    for (size_t i = 0; i < paths.len; i++) {
        const char *path = paths.items[i];
        Doc *open = doc_by_path(s, path);
        PlumLspAnalysis *own = NULL;
        const PlumLspAnalysis *a = open != NULL ? open->analysis : NULL;
        if (open == NULL) {
            size_t len = 0;
            int err = 0;
            char *text = plum_read_file(path, &len, &err);
            if (text == NULL) {
                continue;
            }
            own = plum_lsp_analyze(path, text, len, read_module, s);
            free(text); /* plum_read_file's text is malloc'd */
            a = own;
        }
        PlumLspRanges found = {0};
        if (a != NULL) {
            plum_lsp_references(a, t, declaration, &found);
        }
        if (found.len > 0) {
            char *uri = open != NULL ? plum_strdup(open->uri) : plum_lsp_path_to_uri(path);
            PlumLspLines lines;
            plum_lsp_lines_init(&lines, a->main->text, a->main->len);
            if (new_name != NULL) {
                plum_jw_key(w, uri);
                plum_jw_array(w);
            }
            for (size_t k = 0; k < found.len; k++) {
                plum_jw_object(w);
                if (new_name == NULL) {
                    plum_jw_key(w, "uri");
                    plum_jw_string(w, uri);
                }
                plum_jw_key(w, "range");
                write_range(w, &lines, found.items[k].start, found.items[k].end);
                if (new_name != NULL) {
                    plum_jw_key(w, "newText");
                    plum_jw_string(w, new_name);
                }
                plum_jw_object_end(w);
            }
            if (new_name != NULL) {
                plum_jw_array_end(w);
            }
            plum_lsp_lines_free(&lines);
            plum_free(uri);
        }
        plum_array_free(&found);
        plum_lsp_analysis_free(own);
    }
    if (new_name != NULL) {
        plum_jw_object_end(w);
        plum_jw_object_end(w);
    } else {
        plum_jw_array_end(w);
    }
    free_paths(&paths);
}

/* Quick fixes for the diagnostics in the range asked about, unless the
 * editor asks only for kinds of action other than quick fixes. */
static void code_actions(Server *s, PlumJsonWriter *w, const Doc *d, const PlumJson *params) {
    const PlumJson *only = plum_json_get(plum_json_get(params, "context"), "only");
    bool wanted = only == NULL || only->kind != PLUM_JSON_ARRAY;
    for (size_t i = 0; !wanted && i < only->as.arr.count; i++) {
        const PlumJson *k = only->as.arr.items[i];
        wanted = k->kind == PLUM_JSON_STRING && strncmp(k->as.str.chars, "quickfix", 8) == 0 &&
                 (k->as.str.chars[8] == '\0' || k->as.str.chars[8] == '.');
        wanted = wanted || (k->kind == PLUM_JSON_STRING && k->as.str.len == 0);
    }
    const PlumJson *range = plum_json_get(params, "range");
    if (!wanted || range == NULL) {
        plum_jw_array(w);
        plum_jw_array_end(w);
        return;
    }
    size_t start = offset_of(d, plum_json_get(range, "start"));
    size_t end = offset_of(d, plum_json_get(range, "end"));
    plum_lsp_code_actions(w, d->analysis, d->uri, start, end < start ? start : end, read_module, s);
}

/* prepareRename: the name and its range; rename: the edits. A name that
 * can't be renamed is an error, with the reason for the editor to show. */
static void rename_request(Server *s, const PlumJson *id, const PlumJson *params, const Doc *d,
                           size_t offset, bool rename) {
    PlumLspTarget t;
    bool found = plum_lsp_target(d->analysis, offset, &t);
    if (!found || t.no_rename[0] != '\0') {
        send_error(s, id, ERR_REQUEST_FAILED, t.no_rename);
        plum_lsp_target_free(&t);
        return;
    }
    size_t len = 0;
    const char *new_name = rename ? plum_json_str(params, "newName", &len) : NULL;
    char why[160];
    if (rename && new_name == NULL) {
        send_error(s, id, ERR_INVALID_PARAMS, "missing newName");
        plum_lsp_target_free(&t);
        return;
    }
    if (rename && !plum_lsp_valid_name(new_name, len, why, sizeof why)) {
        send_error(s, id, ERR_INVALID_PARAMS, why);
        plum_lsp_target_free(&t);
        return;
    }
    PlumJsonWriter w;
    response_start(&w, id);
    plum_jw_key(&w, "result");
    if (rename) {
        references(s, &w, d, &t, true, new_name);
    } else {
        PlumLspLines lines;
        plum_lsp_lines_init(&lines, d->text, d->len);
        plum_jw_object(&w);
        plum_jw_key(&w, "range");
        write_range(&w, &lines, t.at_start, t.at_end);
        plum_jw_key(&w, "placeholder");
        plum_jw_stringn(&w, d->text + t.at_start, t.at_end - t.at_start);
        plum_jw_object_end(&w);
        plum_lsp_lines_free(&lines);
    }
    plum_jw_object_end(&w);
    send(s, &w);
    plum_jw_free(&w);
    plum_lsp_target_free(&t);
}

static void request(Server *s, const char *method, const PlumJson *id, const PlumJson *params) {
    if (s->shutdown) {
        send_error(s, id, ERR_INVALID_REQUEST, "the server is shutting down");
        return;
    }
    if (strcmp(method, "initialize") == 0) {
        s->initialized = true;
        PlumJsonWriter w;
        response_start(&w, id);
        plum_jw_key(&w, "result");
        initialize_result(&w);
        plum_jw_object_end(&w);
        send(s, &w);
        plum_jw_free(&w);
        return;
    }
    if (!s->initialized) {
        send_error(s, id, ERR_NOT_INITIALIZED, "the server is not initialized yet");
        return;
    }
    if (strcmp(method, "shutdown") == 0) {
        s->shutdown = true;
        PlumJsonWriter w;
        response_start(&w, id);
        plum_jw_key(&w, "result");
        plum_jw_null(&w);
        plum_jw_object_end(&w);
        send(s, &w);
        plum_jw_free(&w);
        return;
    }
    bool hover = strcmp(method, "textDocument/hover") == 0;
    bool definition = strcmp(method, "textDocument/definition") == 0;
    bool completion = strcmp(method, "textDocument/completion") == 0;
    bool symbols = strcmp(method, "textDocument/documentSymbol") == 0;
    bool format = strcmp(method, "textDocument/formatting") == 0;
    bool refs = strcmp(method, "textDocument/references") == 0;
    bool prepare = strcmp(method, "textDocument/prepareRename") == 0;
    bool rename = strcmp(method, "textDocument/rename") == 0;
    bool signature = strcmp(method, "textDocument/signatureHelp") == 0;
    bool tokens = strcmp(method, "textDocument/semanticTokens/full") == 0;
    bool actions = strcmp(method, "textDocument/codeAction") == 0;
    if (!hover && !definition && !completion && !symbols && !format && !refs && !prepare &&
        !rename && !signature && !tokens && !actions) {
        send_error(s, id, ERR_METHOD_NOT_FOUND, "unknown method");
        return;
    }
    const char *uri = doc_uri(params);
    if (uri == NULL) {
        send_error(s, id, ERR_INVALID_PARAMS, "missing textDocument.uri");
        return;
    }
    const PlumJson *pos = plum_json_get(params, "position");
    if ((hover || definition || completion || refs || prepare || rename || signature) &&
        (pos == NULL || pos->kind != PLUM_JSON_OBJECT)) {
        send_error(s, id, ERR_INVALID_PARAMS, "missing position");
        return;
    }
    Doc *d = doc_by_uri(s, uri);
    size_t offset = 0;
    if (d != NULL && pos != NULL) {
        offset = offset_of(d, pos);
    }
    if ((prepare || rename) && d != NULL && d->analysis != NULL) {
        rename_request(s, id, params, d, offset, rename);
        return;
    }
    PlumJsonWriter w;
    response_start(&w, id);
    plum_jw_key(&w, "result");
    if (d == NULL || d->analysis == NULL) {
        plum_jw_null(&w); /* not a file the editor opened */
    } else if (hover) {
        plum_lsp_hover(&w, d->analysis, offset);
    } else if (definition) {
        PlumLspNamer namer = {uri_of, s};
        plum_lsp_definition(&w, d->analysis, offset, &namer);
    } else if (completion) {
        plum_lsp_completion(&w, d->analysis, d->text, d->len, offset, read_module, s);
    } else if (symbols) {
        plum_lsp_document_symbols(&w, d->analysis);
    } else if (refs) {
        PlumLspTarget t;
        if (plum_lsp_target(d->analysis, offset, &t)) {
            const PlumJson *ctx = plum_json_get(params, "context");
            const PlumJson *decl = plum_json_get(ctx, "includeDeclaration");
            references(s, &w, d, &t, decl == NULL || decl->kind != PLUM_JSON_BOOL || decl->as.b,
                       NULL);
        } else {
            plum_jw_array(&w);
            plum_jw_array_end(&w);
        }
        plum_lsp_target_free(&t);
    } else if (actions) {
        code_actions(s, &w, d, params);
    } else if (tokens) {
        plum_lsp_semantic_tokens(&w, d->analysis);
    } else if (signature) {
        plum_lsp_signature_help(&w, d->analysis, d->text, d->len, offset, read_module, s);
    } else if (prepare || rename) {
        plum_jw_null(&w);
    } else {
        formatting(&w, d);
    }
    plum_jw_object_end(&w);
    send(s, &w);
    plum_jw_free(&w);
}

static void notification(Server *s, const char *method, const PlumJson *params) {
    if (strcmp(method, "exit") == 0) {
        s->exited = true;
        return;
    }
    if (!s->initialized || s->shutdown) {
        return; /* notifications before initialize, or after shutdown, are dropped */
    }
    if (strcmp(method, "textDocument/didOpen") == 0) {
        did_open(s, params);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        did_change(s, params);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        did_close(s, params);
    }
    /* initialized, didSave, $/cancelRequest, $/setTrace, ...: nothing to do */
}

static void handle(Server *s, const char *body, size_t len) {
    PlumArena arena;
    plum_arena_init(&arena);
    PlumJson *msg = plum_json_parse(&arena, body, len);
    if (msg == NULL) {
        send_error(s, NULL, ERR_PARSE, "the message isn't valid JSON");
    } else if (msg->kind != PLUM_JSON_OBJECT) {
        send_error(s, NULL, ERR_INVALID_REQUEST, "a message is a JSON object");
    } else {
        const PlumJson *id = plum_json_get(msg, "id");
        const PlumJson *method = plum_json_get(msg, "method");
        const PlumJson *params = plum_json_get(msg, "params");
        bool id_ok = id == NULL || id->kind == PLUM_JSON_NUMBER || id->kind == PLUM_JSON_STRING;
        if (method == NULL && id != NULL &&
            (plum_json_get(msg, "result") != NULL || plum_json_get(msg, "error") != NULL)) {
            /* a response: the server sends no requests, so it expects none */
        } else if (method == NULL || method->kind != PLUM_JSON_STRING || !id_ok) {
            send_error(s, id_ok ? id : NULL, ERR_INVALID_REQUEST,
                       "a request needs a method (a string) and an id (a number or a string)");
        } else if (id != NULL) {
            request(s, method->as.str.chars, id, params);
        } else {
            notification(s, method->as.str.chars, params);
        }
    }
    plum_arena_free(&arena);
}

int plum_lsp_serve(FILE *in, FILE *out) {
    Server s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.out = out;
    while (!s.exited) {
        char *body = NULL;
        size_t len = 0;
        int got = read_message(&s, &body, &len);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            send_error(&s, NULL, ERR_PARSE, "a message needs a Content-Length header");
            continue;
        }
        handle(&s, body, len);
        plum_free(body);
    }
    for (size_t i = 0; i < s.docs.len; i++) {
        doc_free(s.docs.items[i]);
    }
    plum_array_free(&s.docs);
    for (size_t i = 0; i < s.extra.len; i++) {
        plum_free(s.extra.items[i].uri);
        plum_free(s.extra.items[i].json);
    }
    plum_array_free(&s.extra);
    return s.shutdown ? 0 : 1;
}

int plum_lsp_command(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        /* editors often pass --stdio; it is the only way the server talks */
        if (strcmp(argv[i], "--stdio") != 0) {
            fprintf(stderr,
                    "plum: error: unknown option '%s'\n"
                    "  hint: plum lsp (it talks on standard input and output)\n",
                    argv[i]);
            return 2;
        }
    }
#ifdef _WIN32
    /* Content-Length counts bytes: no "\r\n" translation */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return plum_lsp_serve(stdin, stdout);
}
