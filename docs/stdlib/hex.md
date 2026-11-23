# hex

Bytes as hexadecimal text, two digits a byte, and back: for showing hashes
and binary data, and for protocols that send hex.

```plum
seed hex

drop(hex.encode("hi!"))               # 686921
drop(hex.decode("686921"))            # hi!
drop(hex.encode("z~", upper: good))   # 7A7E
drop(hex.encode("é"))                 # c3a9: each byte of the UTF-8
```

`encode` writes small letters unless `upper` is `good`. `decode` takes
either case, even mixed, and no spaces, `0x` or other separators. What
`decode` gives is bytes, which needn't be UTF-8 text.

## Errors

- `ValueError` from `decode`: an odd number of digits, or a character
  that isn't a hex digit (the message gives its position), like
  `hex.decode: 'z' at position 0 isn't a hex digit`.
- `ValueError` from `encode` if the result would be longer than a `str`
  can hold.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `encode` | `encode(data: str, upper: bool = bad) -> str` | two hex digits for each byte, small letters unless upper |
| `decode` | `decode(text: str) -> str` | pairs of hex digits (either case) back to bytes |
<!-- reference:end -->
