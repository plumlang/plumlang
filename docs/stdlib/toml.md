# toml

Read and write [TOML](https://toml.io/en/v1.0.0) 1.0, the configuration
format `plum.toml` is written in. `parse` is the parser `plum` reads
`plum.toml` with ([Orchard](../orchard.md)), here reading all of TOML
rather than the part a manifest uses.

```plum
seed toml
seed fs

plant config = toml.parse(fs.read("config.toml"))
drop(config.get("server").get("port").int() ?? 8080)

plant text = toml.stringify({"title": "notes", "server": {"host": "localhost", "port": 8080}})
drop(text)
# title = "notes"
#
# [server]
# host = "localhost"
# port = 8080
```

## Reading

`parse` gives a `Json` value, the same kind `json.parse` gives (see
[`Json` values](json.value.md)): the document is a map, and

| TOML | Plumlang |
|---|---|
| table, inline table | `map[str, Json]`, keys in the order they came |
| array, array of tables | `list[Json]` |
| string (all four kinds) | `str` |
| integer (decimal, `0x`, `0o`, `0b`) | `int` |
| float (also `inf` and `nan`) | `float` |
| boolean | `bool` |
| date-time, local date-time, local date, local time | `str`, its RFC 3339 text |

Plumlang has no date type yet, so dates and times come back as their
text, written the same whichever way the file wrote them: `T` between the
date and the time (where the file may have `t` or a space) and `Z` in
capitals, so `1979-05-27 07:32:00z` is `"1979-05-27T07:32:00Z"`. Their
fraction of a second stays as written. They are checked first: a day that
doesn't exist, like `2023-02-29`, is an error.

Everything in TOML 1.0 is read: dotted keys, quoted and empty keys,
multi-line strings (with line-ending backslashes), `\u` and `\U`
escapes (`\u0000` too), integers with `_` between digits, the whole
64-bit range, and the rules on defining a table once, such as that a
table made by dotted keys can't be given a `[header]` later and an inline
table can't be added to. It passes every case of the
[toml-test](https://github.com/toml-lang/toml-test) suite for TOML 1.0
that `tools/toml-test/run.py` runs.

## Writing

`stringify` writes a map as a TOML document: first the keys whose values
aren't tables, then each map value as a `[table]`, then each list of maps
as an `[[array of tables]]`, each under its full dotted name and after a
blank line. Other values are written inline: lists as `[1, 2]`, maps
inside lists as `{ key = value }`. Keys are written bare when they are
letters, digits, `_` and `-`, and quoted otherwise; strings are always
basic strings, with escapes for `"`, `\` and control characters. `parse`
reads the result back as the same map, except that a date read from TOML
is written back as a string.

## Errors

- `ParseError` from `parse`, with the line and column, like
  `toml.parse: line 2, column 1: 'a' is already set on line 1; keep one`.
  Every invalid document is one: a syntax error, a value TOML doesn't
  have (`01`, `1.`, `07:32`), a key or table defined twice, an integer
  past 64 bits, text that isn't UTF-8.
- `ValueError` from `stringify` when the value isn't a map, or holds
  something TOML can't write: `none` (TOML has no null), a set, a map
  with keys that aren't `str`, a function, or text that isn't UTF-8.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `parse` | `parse(text: str) -> Json` | TOML 1.0 text as a map: tables are maps, arrays lists; dates and times are their text; a ParseError says where it went wrong |
| `stringify` | `stringify(value: Json) -> str` | a map as TOML text: plain values first, then [tables] and [[arrays of tables]] |
<!-- reference:end -->
