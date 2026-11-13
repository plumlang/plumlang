<h1 align="center">Plumlang</h1>

<p align="center">
  A small, fast, memory-safe programming language. Easy to write, one binary, runs anywhere.<br>
  An <a href="https://elixpo.com">Elixpo</a> production.
</p>

<p align="center">
  <a href="LICENSE"><img alt="Licence: Plumlang Commercial 1.0" src="https://img.shields.io/badge/licence-Plumlang%20Commercial%201.0-6b3fa0?style=flat-square"></a>
  <a href="https://github.com/plumlang/plumlang/actions/workflows/build.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/plumlang/plumlang/build.yml?branch=main&style=flat-square&label=build"></a>
  <a href="https://github.com/plumlang/plumlang/issues/36"><img alt="Status: pre-alpha" src="https://img.shields.io/badge/status-pre--alpha-orange?style=flat-square"></a>
</p>

---

> **Status:** pre-alpha. The language is being built in the open, one pull
> request at a time. Follow the roadmap in
> [#36](https://github.com/plumlang/plumlang/issues/36).

## Why Plumlang

- **Easy to write.** A small syntax that reads like plain English, with error
  messages that point at the problem and say how to fix it.
- **Memory-safe without a garbage collector.** Reference counting with
  copy-on-write values frees memory the moment it is no longer used: no
  pauses, no manual `free`, no borrow checker to fight.
- **Everything a language needs.** Static types with inference, functions and
  closures, classes and interfaces, lists and maps, modules, error handling,
  files, networking and an HTTP server in the standard library.
- **One binary, runs anywhere.** Written in C11 with no dependencies. The
  same `plum` runs on Linux, macOS, Windows and small boards.

## A taste

The syntax below is the design target; the specification lands in
[#1](https://github.com/plumlang/plumlang/issues/1).

```plum
seed net.http as http

root GREETING = "Hello"

tree Greeter {
    name: str

    branch greet() -> str {
        return "{GREETING}, {self.name}!"
    }
}

plant app = http.server()

app.get("/hello/:name", (req) => {
    plant g = Greeter(req.params["name"])
    return http.text(g.greet())
})

drop("Listening on :8080")
app.listen(8080)
```

## Speed

Plumlang compiles to typed bytecode run by a small virtual machine. On the
[benchmarks](bench/README.md) it runs 1.2 to 2.6 times faster than Python
3.14 on loops, float maths and text, and about even on deep recursion.

## Roadmap

| Stage | What |
|---|---|
| 1. Foundation | Specification, C scaffold, CI, brand, licence, test harness |
| 2. Front end | Lexer, parser, friendly diagnostics |
| 3. Semantic analysis | Scopes, type inference and checking |
| 4. Runtime | Bytecode compiler, virtual machine, reference counting |
| 5. Language and library | Collections, errors, OOP, modules, stdlib, networking, HTTP |
| 6. Tooling and v1.0 | REPL, formatter, editor support, playground, releases |

Every item is an issue; the full list is in
[#36](https://github.com/plumlang/plumlang/issues/36).

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) and the rules in
[PREAMBLE.md](PREAMBLE.md). Coding agents start at [AGENTS.md](AGENTS.md).
Questions go to [Discussions](https://github.com/plumlang/plumlang/discussions)
or **hello@elixpo.com**; see [SUPPORT.md](SUPPORT.md).

## Licence

Plumlang is source-available under the
[Plumlang Commercial License 1.0](LICENSE). It is free for personal,
educational, research and other non-commercial use, and free to evaluate for
60 days. Commercial use, redistribution and hosted services need a commercial
licence: write to **hello@elixpo.com**. Programs you write in Plumlang are
yours. See [PREAMBLE.md](PREAMBLE.md) and [TRADEMARKS.md](TRADEMARKS.md).

---

<p align="center"><sub>© 2026 Elixpo. Plumlang and the Plumlang logo are trademarks of Elixpo.</sub></p>
