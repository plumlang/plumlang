# base64

Bytes as Base64 text (RFC 4648) and back: for HTTP basic auth, data URLs,
and binary data inside JSON or a URL.

```plum
seed base64

drop(base64.encode("hello"))          # aGVsbG8=
drop(base64.decode("aGVsbG8="))       # hello
drop(base64.encode_url("??>"))        # Pz8- (URL- and file-safe, no padding)
drop(base64.decode_url("Pz8-"))       # ??>

plant auth = "Basic " + base64.encode("ada:secret")
```

Strings in Plumlang are bytes, so `encode` takes any text (its UTF-8) or
binary data, and `decode` may give bytes that aren't UTF-8 text; check
them with [`utf8.is_valid`](utf8.md) or look at them with
[`hex.encode`](hex.md).

- `encode` uses the standard alphabet (`A-Z a-z 0-9 + /`) and pads with
  `=` to a multiple of four characters.
- `encode_url` uses `-` and `_` in place of `+` and `/`, and leaves
  the padding out. `decode_url` takes that alphabet with or without
  padding.
- Decoding is strict. Whitespace and line breaks are **rejected**, not
  skipped: take them out first (`text.replace("\n", "")`) if your input
  is wrapped. So are characters of the other alphabet, `=` anywhere but
  the end, more than two `=`, and padding that doesn't make whole groups
  of four. `decode` needs the padding; `decode_url` doesn't.

## Errors

- `ValueError` from `decode` and `decode_url`: a character that isn't
  a Base64 digit (the message gives its position), wrong or missing
  padding, or a last group of one digit, like
  `base64.decode: '!' at position 4 isn't a Base64 digit (A-Z a-z 0-9 + /)`.
- `ValueError` from `encode` and `encode_url` if the result would be
  longer than a `str` can hold.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `encode` | `encode(data: str) -> str` | the bytes as Base64, standard alphabet, with = padding |
| `decode` | `decode(text: str) -> str` | standard Base64 back to bytes; ValueError on a bad digit or padding |
| `encode_url` | `encode_url(data: str) -> str` | the bytes as URL-safe Base64 (- and _ for + and /), without padding |
| `decode_url` | `decode_url(text: str) -> str` | URL-safe Base64 back to bytes, with or without padding |
<!-- reference:end -->
