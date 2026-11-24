# url

Parse, build and resolve URLs, percent-encode text for them, and read and
write query strings, without splitting strings by hand.

```plum
seed url

plant u = url.parse("https://Example.com:8080/docs/../search?q=plum+lang&page=2#top")
drop(u["host"], u["port"], u["path"])  # example.com 8080 /search
drop(u["href"])          # https://example.com:8080/search?q=plum+lang&page=2#top

plant q = url.parse_query(u["query"])
drop(q["q"][0])          # plum lang

drop(url.join("https://example.com/a/b", "../c?x=1"))  # https://example.com/c?x=1
drop(url.build({"scheme": "https", "host": "example.com", "path": "/a file.txt"}))
                         # https://example.com/a%20file.txt
drop(url.build_query({"tag": ["c", "c++"], "q": ["a b"]}))  # tag=c&tag=c%2B%2B&q=a+b
drop(url.encode("a/b c"), url.decode("a%2Fb%20c"))          # a%2Fb%20c a/b c
```

## Parsing

`parse` gives a map with every part as text: `scheme`, `username`,
`password`, `host`, `port`, `path`, `query` (without its `?`),
`fragment` (without its `#`), and `href`, the whole URL written back in
its normal form. A part the URL doesn't have is `""`. `normalize(text)`
is `parse(text)["href"]`.

Parsing follows the [WHATWG URL Standard](https://url.spec.whatwg.org),
the rules browsers use, for what URLs commonly hold:

- Spaces and control characters are trimmed from the ends, and tabs and
  line breaks removed from inside.
- The scheme is lowercased. For the special schemes (`http`, `https`,
  `ws`, `wss`, `ftp` and `file`) so is the host, the scheme's default port
  is dropped (`http://a:80/` is `http://a/`), `\` counts as `/`, and the
  path is at least `/`.
- An IPv6 host is written in its shortest form, in brackets:
  `[2001:DB8:0:0:0:0:0:1]` is `[2001:db8::1]`. A host that ends in a
  number is an IPv4 address and is written as four numbers, whichever
  form browsers take it in: `127.1` and `0x7f.0.0.1` are `127.0.0.1`.
- `.` and `..` path segments are resolved, also when written `%2e`.
- A character a part can't hold as it is (a space, a quote, `<`, `>`,
  anything not ASCII) is percent-encoded, with the set the standard gives
  for that part. `%XX` already in the URL stays as it is, so the path of
  `http://a/x%2Fy` is `/x%2Fy`: `decode` it to get the text.
- A user name and password are encoded too; when the authority holds
  more than one `@`, all but the last belong to them.
- A scheme the standard doesn't know (`mailto:`, `urn:`, `git+ssh:`) keeps
  its case in the host and its path as it is; a path that doesn't start
  with `/`, as in `mailto:a@example.com`, is left whole.
- `file:` URLs have no user name, password or port, and `localhost` as
  their host means none: `file://localhost/x` is `file:///x`.

### Where it differs from the standard

- **No IDNA.** A host with letters outside ASCII has its ASCII letters
  lowercased and stays UTF-8; a browser would write it in punycode
  (`xn--...`).
- **No Windows drive letters** in `file:` URLs: `file:///C:/x/..` is
  `file:///C:/`, where the standard keeps the drive.
- **A URL must have a scheme.** `parse` doesn't take a URL relative to
  another, and `http:example.com` (a special scheme with no slashes) is
  an absolute URL with the host `example.com`, never relative to a base;
  use `join` for relative URLs.
- **An empty query or fragment** (`http://a/?`) is kept in `href` but is
  `""` in the map, the same as no query; `build` writes none.

## Building and joining

`build` takes a map like `parse` gives (only `scheme` is needed; `href`
is ignored, so a map from `parse` can be changed and built again) and
encodes each part as it needs: a `?` or `#` in the path, or a `#` in the
query, becomes `%3F` / `%23`. `%` is left alone, so already-encoded text
stays as it is; encode a literal `%` as `%25`. The result is parsed and
written back in normal form.

`join(base, ref)` reads `ref` relative to `base`, as a browser follows a
link in a page: `"g"`, `"../g"`, `"/g"`, `"?y"`, `"#s"`, `"//host/g"`, or
a whole URL, which replaces the base. It gives what RFC 3986 section 5.4
lists for its examples.

## Percent-encoding and query strings

`encode` percent-encodes every byte except the letters, digits and
`- . _ ~` (RFC 3986's unreserved characters), so its result is safe as
one path segment, query name or query value. `decode` turns each `%XX`
back into its byte, leaving a `%` without two hex digits after it as it
is; `+` stays `+`. What `decode` gives is bytes, which needn't be UTF-8.

`parse_query` reads `application/x-www-form-urlencoded` text, as forms
and query strings use: pairs split on `&`, then on the first `=`; `+` is
a space and `%XX` a byte. It gives each name's values in the order they
came, so a repeated name (`tag=a&tag=b`) keeps all of them, and a name
with no `=` has the value `""`. A leading `?` is skipped. `build_query`
writes the same form back, in the map's order, encoding everything but
letters, digits and `* - . _` (a space becomes `+`).

## Errors

- `ValueError` from `parse`, `normalize`, `build` and `join` when the
  text isn't a URL, saying why: it has no scheme, a special URL has no
  host, a port isn't a number from 0 to 65535, an IPv6 or IPv4 host is
  malformed, a host holds a character no host may (a space, `<`, `%`
  after decoding, ...), or a `file:` URL has a user name or port. Like
  `url.parse: "http://a:8x/" isn't a URL: the port must be a number, not "8x"`.
- `ValueError` from `build` for a map without `scheme` or with a key that
  isn't one of the parts.
- `ValueError` from `join` when the base has an opaque path, like
  `mailto:a@example.com`, and the reference is more than a `#fragment`.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `parse` | `parse(text: str) -> map[str, str]` | the parts of a URL: scheme, username, password, host, port, path, query, fragment, and href (the whole URL, normalized); a missing part is "" |
| `build` | `build(parts: map[str, str]) -> str` | a URL from parts like parse gives (scheme needed, the rest optional), each percent-encoded as it needs |
| `normalize` | `normalize(text: str) -> str` | the URL written as parse reads it: scheme and host in small letters, . and .. resolved, the default port dropped |
| `join` | `join(base: str, ref: str) -> str` | ref read relative to base, as a browser follows a link: "../b", "/c", "?q", "#f", "//host/x" or a whole URL |
| `encode` | `encode(text: str) -> str` | percent-encode every byte but A-Z a-z 0-9 - . _ ~, for one part of a path or query |
| `decode` | `decode(text: str) -> str` | %XX back to bytes (+ stays +); a % without two hex digits is kept |
| `parse_query` | `parse_query(text: str) -> map[str, list[str]]` | a query string (with or without ?) as each name's values in order; + is a space |
| `build_query` | `build_query(params: map[str, list[str]]) -> str` | name=value pairs joined by &, form-encoded (a space is +), in the map's order |
<!-- reference:end -->
