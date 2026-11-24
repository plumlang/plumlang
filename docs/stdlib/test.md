# test

Helpers for the tests that `plum test` runs ([testing](../test.md)).
`seed test` makes it `test`. Most checks need only the built-in
`assert(condition, message)`; this module adds what `assert` can't say on
its own. It is written in Plumlang, in `src/stdlib/plum/test.plum`.

```plum
seed test

branch test_bad_number() {
    plant e = test.throws(() => {
        int("12x")
    })
    assert(e is ParseError, "got a {type_of(e)}")
}
```

## Reference

| Name | Signature | What it does |
|---|---|---|
| `throws` | `throws(f: () -> none) -> Error` | run `f` and return the error it throws; if it throws none, throw an `AssertError` ("expected an error, but none was thrown") |

Check which error came back with `is` (`e is ParseError`, or your own
error tree); its `message` is `e.message`.
