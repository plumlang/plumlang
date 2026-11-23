# path

File paths as text: put them together and take them apart. Nothing here
touches the file system (that's [`fs`](fs.md)); the functions only look
at the text, so they work on paths that don't exist yet.

```plum
seed path

plant file = path.join(["reports", "2026", "summary.csv"])
drop(file)                                # reports/2026/summary.csv (\ on Windows)
drop(path.parent(file))                   # reports/2026
drop(path.name(file), path.stem(file))    # summary.csv summary
drop(path.extension(file))                # csv
drop(path.is_absolute("/etc/hosts"))      # good
```

- The separator is `/`. On Windows a backslash is one too, and `join`
  puts a backslash between parts there; `path.SEPARATOR` is the one
  `join` uses.
- `join` skips empty parts and never doubles a separator: `["a/",
  "/b"]` gives `a/b`. A later part that starts with a separator doesn't
  start the path over; it is joined like any other.
- A path's root (`/`, or on Windows a drive such as `C:\`) is never
  taken apart: the parent of `/` is `/`, and its name is `""`.
  Separators at the end are ignored: the name of `a/b/` is `b`.
- The parent of a plain name (`notes.txt`) is `""`, which `join`
  skips, so `path.join([path.parent(p), "other.txt"])` stays relative.
- The extension is what follows the name's last dot, without the dot. A
  name that starts with its only dot (`.bashrc`) or ends with a dot has
  none. `stem` is the name without the extension and its dot, so
  `archive.tar.gz` has stem `archive.tar` and extension `gz`.
- `is_absolute` is true for a path that starts with a separator, or on
  Windows with a drive and a separator (`C:\x`). `C:x` is relative.

## Errors

None: every string is a path. `join` throws `ValueError` only for a
result longer than a `str` can hold.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `join` | `join(parts: list[str]) -> str` | the parts joined with the separator, never doubling it; empty parts are skipped |
| `parent` | `parent(p: str) -> str` | the folder that holds p: "a/b/c.txt" gives "a/b", "c.txt" gives "" |
| `name` | `name(p: str) -> str` | the last part: "a/b/c.txt" gives "c.txt" |
| `stem` | `stem(p: str) -> str` | the last part without its extension: "a/b/c.txt" gives "c" |
| `extension` | `extension(p: str) -> str` | the extension without its dot: "a/b/c.txt" gives "txt" ("" if none) |
| `is_absolute` | `is_absolute(p: str) -> bool` | does p start with a separator (on Windows, also a drive and one: C:/)? |
| `SEPARATOR` | `SEPARATOR: str` | what join puts between parts: a slash, or a backslash on Windows |
<!-- reference:end -->
