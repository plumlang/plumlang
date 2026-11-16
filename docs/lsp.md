# The language server: `plum lsp`

`plum lsp` is a [Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
server built into the `plum` binary. An editor starts it and talks to it
on standard input and output; there is nothing else to install. It
checks files as `plum check` does, without compiling or running them.

```
plum lsp            # the server, on standard input and output
plum lsp --stdio    # the same (some editors add the flag)
```

## What it does

| Capability | What you get |
|---|---|
| Diagnostics | Every error and warning `plum check` reports, with the same code (`E0402`, `W0502`, ...), message and place, on opening a file and on every change. A file you import that isn't open gets its own diagnostics too. |
| Hover | The type of the name or expression under the cursor; the signature of functions, methods and standard-library functions, with their one-line documentation; a tree's, trait's or kind's header; a field's declaration. |
| Go to definition | Local variables, parameters, loop names, functions, trees, traits, kinds and their cases, fields and methods (through the receiver's type, parents included), and names imported from other files: `seed ./shapes { Circle }`, `shapes.area`, and the `./shapes` of the `seed` itself. |
| Completion | The names in scope at the cursor (locals, parameters, top-level names, imports, built-ins and keywords). After a `.`: a tree's fields and methods, the methods of `list`, `map`, `set`, `str`, `Json`, tasks, channels and sockets, a kind's cases, and the functions and constants of a standard module or the names a file module exports. |
| Document symbols | The outline: functions, trees and traits with their fields and methods, kinds with their cases, top-level variables, constants and imports. |
| Formatting | The whole file in `plum fmt`'s layout (spec 14.1). A file with syntax errors is left alone. |

Details:

- **Text sync** is the whole text on every change (`TextDocumentSyncKind.Full`).
  Each change is checked at once; a file of a few thousand lines takes
  milliseconds.
- **Unsaved files.** When an open file imports another file that is also
  open, it sees the editor's text of it, saved or not. Changing a file
  checks again the open files that import it.
- **Half-typed code.** Where `plum check` stops at the first file with
  errors, the server goes on, so names still resolve and hover and
  completion still work on a line you are in the middle of typing. A file
  whose names don't all resolve has no types, so completion after a dot
  then offers nothing for values, but still offers a module's members and
  a kind's cases.
- **Positions** are in UTF-16 code units, the protocol's default.
- **Robustness.** Malformed JSON, a message without `Content-Length`,
  unknown methods (answered with `MethodNotFound`, `-32601`) and requests
  before `initialize` are answered with an error, and the server carries
  on. It exits on `exit`, with 0 after a `shutdown` request and 1 without
  one, or when its input ends.

## Editors

The server needs only the path of `plum`; with `plum` on your `PATH`, the
command is `plum lsp`. Associate the `.plum` extension with a `plum`
language.

### VS Code

Install the **Plumlang** extension (from the Marketplace or Open VSX). It
starts `plum lsp` itself. Settings:

| Setting | Default | |
|---|---|---|
| `plum.path` | `plum` | the `plum` binary, if it isn't on `PATH` |
| `plum.lsp.enabled` | `true` | turn the language server off (highlighting stays) |

### Neovim (0.11 and later, built-in LSP)

```lua
vim.filetype.add({ extension = { plum = "plum" } })

vim.lsp.config("plum", {
  cmd = { "plum", "lsp" },
  filetypes = { "plum" },
  root_markers = { "plum.toml", ".git" },
})
vim.lsp.enable("plum")
```

On Neovim 0.10, start it from an autocommand instead:

```lua
vim.api.nvim_create_autocmd("FileType", {
  pattern = "plum",
  callback = function(args)
    vim.lsp.start({
      name = "plum",
      cmd = { "plum", "lsp" },
      root_dir = vim.fs.root(args.buf, { "plum.toml", ".git" }),
    })
  end,
})
```

Formatting on save: `vim.lsp.buf.format()` in a `BufWritePre` autocommand.

### Helix

In `~/.config/helix/languages.toml`:

```toml
[language-server.plum]
command = "plum"
args = ["lsp"]

[[language]]
name = "plum"
scope = "source.plum"
file-types = ["plum"]
roots = ["plum.toml"]
comment-token = "#"
indent = { tab-width = 4, unit = "    " }
language-servers = ["plum"]
auto-format = true
```

### Zed

In `settings.json`, until there is a Zed extension for Plumlang:

```json
{
  "file_types": { "Plumlang": ["plum"] },
  "lsp": {
    "plum": { "binary": { "path": "plum", "arguments": ["lsp"] } }
  },
  "languages": {
    "Plumlang": { "language_servers": ["plum"], "format_on_save": "language_server" }
  }
}
```

Zed attaches a language server to a language that an extension defines,
so this needs a minimal language extension (a `config.toml` with
`name = "Plumlang"`, `path_suffixes = ["plum"]`, `line_comments = ["# "]`)
installed with *zed: install dev extension*.

### Emacs (eglot)

```elisp
(define-derived-mode plum-mode prog-mode "Plum"
  (setq-local comment-start "# "))
(add-to-list 'auto-mode-alist '("\\.plum\\'" . plum-mode))

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs '(plum-mode "plum" "lsp")))
(add-hook 'plum-mode-hook #'eglot-ensure)
```

### Any other editor

Run `plum lsp` as the server command for `.plum` files, talking over
stdio. No initialization options are needed.

## How it works

The server lives in `src/lsp/`: `server.c` reads and writes messages and
keeps the open files, `analysis.c` checks a file and answers questions
about it, `json.c` and `text.c` are its JSON and its positions.

A file is lexed, parsed, resolved and type-checked with the same code
`plum check` uses, into the same diagnostics; the compiler and the VM are
never involved. The resolver already links every name in the syntax tree
to its symbol, which knows where it was declared, and the type checker
gives every expression and symbol its type. So the server keeps no extra
table while checking: it answers each request by walking the checked
tree to the name or expression at the cursor. Checking for `plum run`
costs exactly what it did before.

Completion after a dot checks the file once more with a placeholder name
after the dot, so the value before the dot has a type even while the
line is incomplete.

The tests are recorded sessions: `tests/lsp/*.in`, run by `tests/lsp.sh`
and compared with `tests/lsp/*.out`.
