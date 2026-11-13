# str

The methods of strings. They need no `seed`:

```plum
plant line = "  name = Ada  "
plant parts = line.split("=")
drop(parts[0].trim(), parts[1].trim().upper())   # name ADA
drop("plum".pad_start(6, fill: "*"))             # **plum
```

Strings are UTF-8. Positions and lengths count characters, as indexing
does (`find` gives a character position). `upper`, `lower` and the `is_`
tests know ASCII letters, digits and whitespace only.

## Errors

- `ValueError`: `split("")`, `count("")` or `replace("", ...)` (the text
  to look for is empty); `repeat` with a negative count or a result too
  long; `pad_start` / `pad_end` with a fill that isn't one character.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `upper` | `upper(self: str) -> str` | the text in capitals (ASCII letters) |
| `lower` | `lower(self: str) -> str` | the text in small letters (ASCII letters) |
| `trim` | `trim(self: str) -> str` | without whitespace at either end |
| `trim_start` | `trim_start(self: str) -> str` | without whitespace at the start |
| `trim_end` | `trim_end(self: str) -> str` | without whitespace at the end |
| `split` | `split(self: str, separator: str = "") -> list[str]` | the pieces between separators; with none given, the words between whitespace |
| `lines` | `lines(self: str) -> list[str]` | the lines, without their line breaks |
| `chars` | `chars(self: str) -> list[str]` | each character as a str |
| `contains` | `contains(self: str, part: str) -> bool` | does the text contain part? |
| `starts_with` | `starts_with(self: str, prefix: str) -> bool` | does the text start with prefix? |
| `ends_with` | `ends_with(self: str, suffix: str) -> bool` | does the text end with suffix? |
| `find` | `find(self: str, part: str) -> int?` | the position of the first part, or none |
| `count` | `count(self: str, part: str) -> int` | how many times part appears |
| `replace` | `replace(self: str, old: str, new: str) -> str` | every old replaced with new |
| `repeat` | `repeat(self: str, times: int) -> str` | the text that many times over |
| `reverse` | `reverse(self: str) -> str` | the characters in reverse order |
| `pad_start` | `pad_start(self: str, width: int, fill: str = " ") -> str` | filled at the start up to width characters |
| `pad_end` | `pad_end(self: str, width: int, fill: str = " ") -> str` | filled at the end up to width characters |
| `is_digit` | `is_digit(self: str) -> bool` | is every character 0 to 9? |
| `is_alpha` | `is_alpha(self: str) -> bool` | is every character a letter (ASCII)? |
| `is_space` | `is_space(self: str) -> bool` | is every character whitespace? |
<!-- reference:end -->
