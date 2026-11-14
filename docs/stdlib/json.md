# json

Parse and produce JSON. A parsed document is a `Json` value: take it
apart with its methods ([`json.value`](json.value.md)), which give `none`
or null instead of failing, so a lookup can go several levels deep and
end with `??`:

```plum
seed json

plant data = json.parse(fs.read("config.json"))
plant port = data.get("server").get("port").int() ?? 8080
for user in data.get("users").list() ?? [] {
    drop(user.get("name").str() ?? "(no name)")
}
```

Anything made of plain data can be a `Json`: numbers, strings, bools,
`none`, lists, `map[str, ...]`, and tree objects (written as their
fields). A list or map literal whose expected type is `Json` may mix
types:

```plum
plant body: Json = {"name": "Ada", "age": 36, "tags": ["admin", 1]}
drop(json.stringify(body))             # {"name":"Ada","age":36,"tags":["admin",1]}
drop(json.stringify(body, indent: 2))  # one member per line
```

How JSON and Plumlang values match:

| JSON | Plumlang |
|---|---|
| `null` | `none` |
| `true`, `false` | `good`, `bad` |
| a number without a fraction or exponent that fits | `int` |
| any other number | `float` |
| a string | `str` (escapes, including `\u` pairs, become UTF-8) |
| an array | `list[Json]` |
| an object | `map[str, Json]`, keys in their order |

Writing also takes sets (as arrays) and tree objects (as objects of their
fields; a weak field is written as what it refers to, or null).

## Errors

- `ParseError` from `parse`: the message gives the line and column, like
  `json.parse: line 3, column 9: expected ':' after the key`. Nesting
  deeper than 512 is refused.
- `ValueError` from `stringify`: infinity or NAN, a map whose keys aren't
  strings, a kind's value, a function, or nesting deeper than 512 (an
  object that contains itself).

Writing JSON inside a Plumlang string needs `\{` and `\}`, since braces
start interpolation there; JSON usually comes from a file or the network
instead.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `parse` | `parse(text: str) -> Json` | JSON text as a Json value; a ParseError says where it went wrong |
| `stringify` | `stringify(value: Json, indent: int = 0) -> str` | a value as JSON text, on one line or indented by that many spaces |
<!-- reference:end -->
