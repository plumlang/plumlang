# csv

Read and write comma-separated values (RFC 4180), the format spreadsheets
export. Every field is text; convert with `int()` or `float()` as needed.

```plum
seed csv

plant rows = csv.parse("name,age\nAda,36\n\"Bo, Jr.\",7\n")
drop(rows)                    # [["name", "age"], ["Ada", "36"], ["Bo, Jr.", "7"]]

plant people = csv.parse_records("name,age\nAda,36\n")
drop(people[0]["name"])       # Ada (the first row names the fields)

drop(csv.stringify([["a", "b,c"], ["1", "say \"hi\""]]))
# a,"b,c"
# 1,"say ""hi"""
```

Reading:

- A field in double quotes may hold the separator, line breaks, and `""`
  for one quote. A quote in the middle of an unquoted field is just a
  character.
- Lines end with `\n`, `\r\n` or `\r`; the last line needn't end
  with one.
- Empty fields are `""`. Empty lines are skipped, so they make no rows.
- `separator` is any one ASCII character but a quote or a line break:
  `";"` and `"\t"` are common.
- `parse_records` takes the first row as the field names and gives a map
  for each row after it. A row with fewer fields than the header gets
  `""` for the rest; a row with more is a `ValueError`, as is a header
  that names a field twice. Text with only a header gives `[]`.

Writing:

- `stringify` quotes a field only when it must: when it holds the
  separator, a quote or a line break. A row of one empty field is written
  `""`, so it doesn't read back as an empty line.
- Every row ends with `\n`.
- `csv.parse(csv.stringify(rows)) == rows` for any rows that have at
  least one field each (a row with no fields is written as an empty line,
  which reading skips).

## Errors

- `ParseError` from `parse` and `parse_records`: a quoted field with
  no closing quote, or text after a closing quote, with the line, like
  `csv.parse: line 3: the quoted field has no closing quote`.
- `ValueError`: a separator that isn't one character (or is a quote or a
  line break); in `parse_records`, a row with more fields than the header,
  or a field named twice; from `stringify`, a result longer than a
  `str` can hold.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `parse` | `parse(text: str, separator: str = ",") -> list[list[str]]` | every row, as a list of its fields (text) |
| `parse_records` | `parse_records(text: str, separator: str = ",") -> list[map[str, str]]` | a map for each row, keyed by the first row; a short row's missing fields are "" |
| `stringify` | `stringify(rows: list[list[str]], separator: str = ",") -> str` | the rows as CSV, fields quoted only when they must be; each row ends with \n |
<!-- reference:end -->
