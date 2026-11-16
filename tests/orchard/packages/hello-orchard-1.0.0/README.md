# hello-orchard

The example package of [Orchard](https://github.com/plumlang/registry),
Plumlang's package registry.

```sh
plum add hello-orchard
```

```plum
seed hello_orchard

drop(hello_orchard.greet("Ada"))   # Hello, Ada, from the orchard!
drop(hello_orchard.cheer("Ada"))   # the same, louder
```
