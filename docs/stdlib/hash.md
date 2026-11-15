# hash

Cryptographic digests of text. A digest is a short fingerprint of some
bytes: the same bytes always give the same digest, and any change, even
of one bit, gives a different one. Use it to check that a download
arrived intact or that two files are the same.

```plum
seed hash

drop(hash.sha256("abc"))
# ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

plant text = fs.read("bundle.plumpkg")
if hash.sha256(text) != expected {
    throw ValueError("the download is corrupt")
}
```

`sha256` hashes the string's bytes (its UTF-8), zero bytes included, and
gives the digest as 64 lowercase hex digits. It comes from the vendored
Mbed TLS. Compare digests with `==`; the comparison is not constant-time,
so don't use it to check secrets such as passwords.

## Errors

- `ValueError` from `sha256` if the crypto library fails to start, which
  shouldn't happen.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `sha256` | `sha256(text: str) -> str` | the SHA-256 digest of the text's bytes, as 64 lowercase hex digits |
<!-- reference:end -->
