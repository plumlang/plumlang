# Json values

The methods of `Json` values (from `json.parse`, or any plain data given
the type `Json`). They need no `seed`. None of them fail: asking a value
for something it isn't gives `none` (for the typed answers) or null (for
`get` and `at`), so lookups chain:

```plum
plant city = data.get("address").get("city").str() ?? "unknown"
plant first = data.get("items").at(0)
drop(first.type_name())     # "object", "list", "string", ...
```

`int()` gives a number only if it is whole (`3.0` gives 3, `3.5` gives
none); `float()` gives any number.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `get` | `get(self: Json, key: str) -> Json` | an object's member; null if it's missing or this isn't an object |
| `at` | `at(self: Json, index: int) -> Json` | a list's item (-1 is the last); null if out of range or this isn't a list |
| `len` | `len(self: Json) -> int` | the items of a list, members of an object or characters of a string; else 0 |
| `keys` | `keys(self: Json) -> list[str]` | an object's keys in order; else [] |
| `type_name` | `type_name(self: Json) -> str` | "null", "bool", "number", "string", "list" or "object" |
| `is_null` | `is_null(self: Json) -> bool` | is this null? |
| `str` | `str(self: Json) -> str?` | the string, or none if this isn't one |
| `int` | `int(self: Json) -> int?` | the number as an int, or none if it isn't a whole one |
| `float` | `float(self: Json) -> float?` | the number as a float, or none |
| `bool` | `bool(self: Json) -> bool?` | the bool, or none |
| `list` | `list(self: Json) -> list[Json]?` | the list, or none |
| `map` | `map(self: Json) -> map[str, Json]?` | the object as a map, or none |
<!-- reference:end -->
