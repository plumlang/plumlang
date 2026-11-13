# The standard library

Modules are imported with `seed` (spec section 9.2):

```plum
seed math                  # math.sqrt(2.0)
seed math as m             # m.sqrt(2.0)
seed math { sqrt, PI }     # sqrt(PI)
```

| Module | Contents |
|---|---|
| [`math`](math.md) | constants and functions on numbers, and random numbers |
| [`str`](str.md) | the methods of strings, always available without `seed` |

`plum stdlib` lists every module's functions with their signatures, and
`plum stdlib math` one module's. The reference table on each page is
generated from the same list (`make stdlib-doc`), and `make test` fails if
a page falls behind.

In signatures, `num` means an `int` or a `float`: a `num` result is an
`int` when every `num` argument is an `int`. A parameter written with
`= default` can be left out.

Errors that the functions throw are listed with each module.
