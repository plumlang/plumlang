# utf8

Characters as numbers (code points), and checking and repairing UTF-8.
Strings in Plumlang hold UTF-8 bytes, and `len` and indexing count
characters; this module gives a character's number, makes a character
from one, and deals with bytes that aren't valid UTF-8, as text from a
file, the network, [`hex.decode`](hex.md) or
[`base64.decode`](base64.md) can be.

```plum
seed utf8

drop(utf8.code("é"))                          # 233
drop(utf8.char(9731))                         # ☃
drop(utf8.codes("hé"))                        # [104, 233]
drop(utf8.from_codes([80, 108, 117, 109]))    # Plum
drop(utf8.is_valid("plum"))                   # good
plant clean = utf8.fix(fs.read("upload.txt")) # bad bytes become U+FFFD (�)
```

- Valid means strict UTF-8 (RFC 3629): no overlong forms (`C0 80` for
  NUL), no surrogates (U+D800 to U+DFFF, encoded as `ED A0 80` and up),
  nothing above U+10FFFF, and no sequence cut short.
- `fix` keeps valid text as it is and replaces each invalid part with one
  U+FFFD, the way the Unicode Standard recommends and browsers do: a lone
  bad byte gives one, a sequence cut short (`E2 82`) gives one, and the
  bytes of an overlong form or a surrogate give one each.
- `code` and `codes` need valid UTF-8; use `fix` first on text that may
  not be.

## Errors

- `ValueError` from `code`: an empty string, more than one character,
  or invalid UTF-8.
- `ValueError` from `char` and `from_codes`: a number below 0, above
  0x10FFFF (1114111), or a surrogate (0xD800 to 0xDFFF); `from_codes`
  says which item, like
  `utf8.from_codes: item 1: 57343 (0xDFFF) is a surrogate, which UTF-8 can't hold`.
- `ValueError` from `codes`: invalid UTF-8, with the byte position.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `code` | `code(ch: str) -> int` | the code point of a one-character string |
| `char` | `char(code: int) -> str` | the character with that code point (0 to 0x10FFFF, no surrogates) |
| `codes` | `codes(text: str) -> list[int]` | the code point of every character |
| `from_codes` | `from_codes(codes: list[int]) -> str` | the text of those code points |
| `is_valid` | `is_valid(text: str) -> bool` | is the text strict UTF-8 (no overlong forms, surrogates or code points past 0x10FFFF)? |
| `fix` | `fix(text: str) -> str` | valid UTF-8: each invalid sequence replaced with U+FFFD |
<!-- reference:end -->
