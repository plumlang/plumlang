# Examples

Small complete programs. Each `NAME.plum` has its expected output in
`NAME.out`, and `make test` runs them all, so they always work with the
current `plum`.

| Example | Shows |
|---|---|
| `bank.plum` | trees with methods, inheritance and `super`, custom errors, `to_str` |
| `shapes.plum` | traits with default methods, `init`, `map` / `filter` / `sorted_by` on objects |
| `todo.plum` | shared objects, lists inside objects, `equals` for `==`, `if plant` |
| `wordcount.plum` | the ten most common words in a file: `os.args`, `fs`, string methods, a map of counts, `compare` for `sorted` |
| `garden/` | a program of several files: `seed ./models/plant`, a `plum.toml` package (`plum run examples/garden`) |
| `config/` | a package that reads a JSON config file and reports every missing or wrong field: `json`, `try` / `catch`, a custom error, optionals |
| `echo.plum` | an echo server and client: sockets and a task per connection |
| `http/hello.plum` | a web server in a few lines: routes, query strings, JSON |
| `http/rest_api.plum` | a REST API with JSON, file-backed storage and an API-key middleware |
| `http/chat.plum` | a TCP chat server: a task per client, a hub task and channels between them |

Run one with `plum run examples/bank.plum`. Some take arguments:
`plum run examples/wordcount.plum notes.txt`, `plum run examples/config
my.json`, `plum run examples/http/chat.plum 9000`; without them they run
on built-in samples, which is what `make test` checks.
