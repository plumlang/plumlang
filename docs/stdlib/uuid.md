# uuid

Universally unique identifiers ([RFC 9562](https://www.rfc-editor.org/rfc/rfc9562)),
for naming records, files and requests without asking anyone else for a
number. A UUID is text here: 32 hex digits in small letters, with hyphens,
like `0190b6a4-5c3e-7d2a-9f1b-3c4d5e6f7a8b`.

```plum
seed uuid

plant id = uuid.v4()                  # 3f0c9a5e-27b1-4c8d-9e02-6b5a41f7d913
plant row = uuid.v7()                 # 01a20c23-c37b-7a41-8b6e-0f52c1d9a7e4
drop(uuid.version(row))               # 7
drop(uuid.timestamp(row))             # 1795500000123: milliseconds since 1970

drop(uuid.parse("URN:UUID:3F0C9A5E-27B1-4C8D-9E02-6B5A41F7D913"))
                                      # 3f0c9a5e-27b1-4c8d-9e02-6b5a41f7d913
drop(uuid.is_valid("not-a-uuid"))     # bad
```

## Which one

- **`v4`** is 122 random bits. Use it when an identifier should say
  nothing about when or where it was made.
- **`v7`** starts with the time in milliseconds, then 74 random bits, so
  identifiers made later sort later, as text too. That keeps database
  indexes compact and lets you sort records by when they were made. Each
  `v7` a program makes sorts after the one before it, even within the same
  millisecond or if the system clock goes back: the random bits then count
  up from the last identifier's by a random step (RFC 9562 section 6.2,
  method 2). It does show when the identifier was made.

The random bits come from the system's secure random source (through the
crypto library `hash` uses), never from `math.random`'s generator, which a
program can seed. Both set the version digit (the 13th, `4` or `7`) and the
variant bits (the 17th digit is `8`, `9`, `a` or `b`).

## Reading and comparing

`parse` takes a UUID in either case, with or without its hyphens, in
`{braces}` or after `urn:uuid:`, and gives it back in the usual form. The
other functions that take a UUID accept the same forms. `version` reads
the version from any UUID, `variant` its variant (`"rfc"` for those RFC
9562 describes), and `compare` orders two by their bytes, which for `v7`
is by time.

`uuid.NIL` (all zeros) and `uuid.MAX` (all ones) are the two special
values RFC 9562 names.

## Errors

- `ValueError` from `parse`, `version`, `variant`, `compare` and
  `timestamp` when the text isn't a UUID in one of the forms above, like
  `uuid.parse: "12-34" isn't a UUID; write 32 hex digits like "0190b6a4-5c3e-7d2a-9f1b-3c4d5e6f7a8b"`.
  `is_valid` says the same without throwing.
- `ValueError` from `timestamp` for a UUID that isn't version 7.
- `ValueError` from `v4` and `v7` in the unlikely case that the system's
  random source fails.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `v4` | `v4() -> str` | a random UUID (version 4), from the system's secure random source |
| `v7` | `v7() -> str` | a time-ordered UUID (version 7): the Unix time in milliseconds, then random bits; each one a program makes sorts after the one before |
| `parse` | `parse(text: str) -> str` | a UUID in its usual form, 36 characters in small letters; takes either case, no hyphens, {braces} or urn:uuid: |
| `is_valid` | `is_valid(text: str) -> bool` | would parse accept the text? |
| `version` | `version(id: str) -> int` | the version number, 0 to 15 (4 for v4, 7 for v7) |
| `variant` | `variant(id: str) -> str` | the variant: "rfc" (RFC 9562, as v4 and v7 make), "ncs", "microsoft" or "future" |
| `compare` | `compare(a: str, b: str) -> int` | -1, 0 or 1 as a sorts before, with or after b, byte by byte (v7 by time) |
| `timestamp` | `timestamp(id: str) -> int` | a version 7 UUID's time, in milliseconds since 1970-01-01 00:00 UTC |
| `NIL` | `NIL: str` | the nil UUID, all zero bits |
| `MAX` | `MAX: str` | the max UUID, all one bits |
<!-- reference:end -->
