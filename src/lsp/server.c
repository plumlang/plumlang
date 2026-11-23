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
#include "util/alloc.h"
#include "util/arena.h"
#include "util/array.h"

#include <errno.h>
#include <stdbool.h>
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

static bool same_path(const char *a, const char *b) {
#ifdef _WIN32
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = *a == '\\' ? '/' : *a;
        char y = *b == '\\' ? '/' : *b;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

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
        if (same_path(s->docs.items[i]->path, path)) {
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
        if (same_path(d->path, path) ||
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
    if (!hover && !definition && !completion && !symbols && !format) {
        send_error(s, id, ERR_METHOD_NOT_FOUND, "unknown method");
        return;
    }
    const char *uri = doc_uri(params);
    if (uri == NULL) {
        send_error(s, id, ERR_INVALID_PARAMS, "missing textDocument.uri");
        return;
    }
    const PlumJson *pos = plum_json_get(params, "position");
    if ((hover || definition || completion) && (pos == NULL || pos->kind != PLUM_JSON_OBJECT)) {
        send_error(s, id, ERR_INVALID_PARAMS, "missing position");
        return;
    }
    Doc *d = doc_by_uri(s, uri);
    size_t offset = 0;
    if (d != NULL && pos != NULL) {
        offset = offset_of(d, pos);
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
