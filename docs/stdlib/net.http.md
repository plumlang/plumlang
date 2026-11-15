# net.http

An HTTP/1.1 server and client. `seed net.http` makes it `http`.

```plum
seed net.http

plant app = http.server()
app.get("/hello/:name", (req) => http.text("Hello, {req.params["name"]}!"))
app.listen(8080)
```

Each connection is served by a task of its own (spec section 12), so a
slow client holds up nobody. The module is written in Plumlang
(`src/stdlib/plum/net_http.plum`) on top of `net`, tasks and `json`, and
built into the binary.

## The server

| Call | What it does |
|---|---|
| `http.server() -> Server` | a server with no routes yet |
| `app.get(pattern, handler)`, `post`, `put`, `patch`, `delete` | a route; `handler` is `(Request) -> Response` |
| `app.route(method, pattern, handler)` | a route for any method |
| `app.use(middleware)` | `(req, next) => ...` runs around every request; call `next(req)` to go on, or return a response yourself |
| `app.static(prefix, dir)` | `GET prefix/a.css` serves `dir/a.css`; `index.html` for a directory; nothing outside `dir` |
| `app.listen(port = 8080, host = "0.0.0.0")` | serve until the program ends |
| `app.start(port = 8080, host = "0.0.0.0") -> int` | serve in the background; the port (0 picks a free one) |
| `app.stop()` | stop accepting connections |
| `app.handle(req) -> Response` | the response to a request, without any network: for tests |
| `app.log_errors` | print handlers' errors to standard error (default `good`) |

Patterns match whole paths, part by part: `/users/:id/posts/:post`
fills `req.params["id"]` and `req.params["post"]`. A path that matches
no route is 404; one that matches with another method is 405. `HEAD` is
answered by the `GET` route, without the body. An error a handler doesn't
catch becomes a 500 response (and is logged).

Connections are kept alive between requests (HTTP/1.1), and closed after
30 seconds without one. Bodies are limited to 16 MB and requests to 100
headers.

### Request

| Field or method | |
|---|---|
| `method`, `path` | `"GET"`, `"/users/7"` |
| `query` | `map[str, str]` of `?a=1&b=two`, decoded |
| `params` | `map[str, str]` from the route pattern |
| `headers` | `map[str, str]`, names in small letters |
| `body` | the body as text |
| `remote` | the client, as `"host:port"` |
| `header(name) -> str?` | one header, any case |
| `json() -> Json` | the body parsed as JSON (a `ParseError` if it isn't) |

### Responses

| Call | |
|---|---|
| `http.text(body, status = 200)` | `text/plain` |
| `http.html(body, status = 200)` | `text/html` |
| `http.json(value, status = 200)` | `value` (any `Json`) as `application/json` |
| `http.redirect(to, status = 302)` | a `location` header |
| `http.status(code, message = "")` | just a status, with its reason or `message` as the body |
| `Response(status, body, headers)` | anything else |

A `Response` has `status`, `body` and `headers`, and `header(name)`,
`with_header(name, value)` (returns itself, for chaining), `json()` and
`ok()` (a 2xx status). It prints as `404 Not Found`.

## The client

| Call | |
|---|---|
| `http.get(url, headers = {:}, timeout = 30.0) -> Response` | |
| `http.post(url, body, content_type = "text/plain; charset=utf-8", headers = {:}, timeout = 30.0)` | |
| `http.post_json(url, value, headers = {:}, timeout = 30.0)` | `value` (any `Json`) as the JSON body |
| `http.request(method, url, body = "", headers = {:}, timeout = 30.0)` | any method |

They give the response whatever its status (check `res.ok()` or
`res.status`). Bodies sent with a length, in chunks, or up to the end of
the connection are all read.

```plum
plant res = http.post_json("http://localhost:8080/notes", {"text": "water the plants"})
if res.ok() {
    drop(res.json().get("id").int())
}
```

## Errors

- `NetError`: the server can't be reached, the connection breaks, a time
  limit runs out, or the answer isn't HTTP.
- `ValueError`: a URL that isn't `http://host[:port]/path`. `https://`
  needs TLS, which is next.

## Speed

`bench/net/http_load.py` drives `bench/net/http_server.plum` with 8
keep-alive connections (Python clients, one process each; Linux x86_64,
GCC `-O2`): about 127,000 text responses and 112,000 JSON responses a
second, 0.06 to 0.07 ms per request at the median, 0.12 to 0.16 ms at
the 99th percentile. The server is one thread; the Python clients are
the limit at one connection (about 20,000 a second).

## Examples

`examples/http/hello.plum` and `examples/http/rest_api.plum` (a JSON API
storing notes in a file, with an API-key middleware).
