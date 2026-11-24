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
| Hover | The type of the name or expression under the cursor; the signature of functions, methods and standard-library functions, with their one-line documentation; a tree's, trait's or kind's header; a field's declaration. A declaration's doc comment (its `##` lines, spec 2.2) follows, as Markdown; on a `seed` of a file, the module's own doc comment. |
| Go to definition | Local variables, parameters, loop names, functions, trees, traits, kinds and their cases, fields and methods (through the receiver's type, parents included), and names imported from other files: `seed ./shapes { Circle }`, `shapes.area`, and the `./shapes` of the `seed` itself. |
| Completion | The names in scope at the cursor (locals, parameters, top-level names, imports, built-ins and keywords). After a `.`: a tree's fields and methods, the methods of `list`, `map`, `set`, `str`, `Json`, tasks, channels and sockets, a kind's cases, and the functions and constants of a standard module or the names a file module exports. |
| Document symbols | The outline: functions, trees and traits with their fields and methods, kinds with their cases, top-level variables, constants and imports. |
| Formatting | The whole file in `plum fmt`'s layout (spec 14.1). A file with syntax errors is left alone. |
| Find references | Every place in the package that names the same variable, parameter, function, tree, trait, kind, case, field, method or module alias: uses, imports (`seed ./beds { Bed }`, `beds.Bed`), keyword arguments (`Bed(plants: 4)` names the field `plants`) and, if asked, the declaration. |
| Rename | Renames all of those at once, in every file of the package, open or not, as one edit. Built-ins (`drop`, `int`, `ValueError`), the standard library, keywords and a module's file name can't be renamed; the editor shows why. The new name must be a name and not a keyword. |
| Signature help | While you type a call's arguments (after `(` and `,`): the signature of the function, method, constructor (`init`'s parameters, or the fields, parents' first), kind case, built-in or standard-library function, with the argument you are on highlighted; a keyword argument (`step: 2`) highlights the parameter it names. Works on a call you are still typing. |
| Semantic tokens | Colour by meaning, for the whole file: what each name stands for (a parameter, a constant, a field, a case, a standard-library function, ...), and whether it is declared there; keywords, numbers, strings and comments too, so a file that doesn't parse is still coloured. The legend is below. |
| Code actions | Quick fixes for diagnostics: **Add `seed math`** for an unknown name that is a standard module (E0301), or **Add `seed ./name`** when `name.plum` is beside the file, placed after the last `seed` (or after the comments the file starts with); **Prefix 'x' with '_' to keep it** for a name never used (W0502). |

### Semantic token legend

The server uses only the protocol's standard token types and modifiers,
so every theme already has colours for them.

| Token type | Plumlang |
|---|---|
| `namespace` | a module: `seed math`, `seed ./shapes as sh`, and `math` in `math.sqrt` |
| `type` | a built-in type: `int`, `float`, `str`, `list`, ... |
| `class` | a tree, and the built-in error trees (`Error`, `ValueError`, ...) |
| `interface` | a trait |
| `enum` | a kind |
| `enumMember` | a kind's case: `Light.Off`, `Dim(level)` in a `match` |
| `parameter` | a parameter of a function, method, lambda or case, and a keyword argument's name |
| `variable` | `plant` and `root` names, loop names, names a pattern or `if plant` binds |
| `property` | a tree's field |
| `function` | a `branch`, a built-in function (`drop`, `len`) or a standard-library function |
| `method` | a method of a tree or trait, and a method of `str`, `list`, `map`, ... |
| `keyword` | `plant`, `branch`, `if`, `good`, `none`, `self`, ... |
| `string`, `number`, `comment` | literals, and `#`, `##` and `#[ ... ]#` comments |

| Modifier | When |
|---|---|
| `declaration` | where the name is declared |
| `readonly` | a `root` constant, or a standard-library constant (`math.PI`) |
| `defaultLibrary` | built into Plumlang: built-in functions, types and errors, and the standard library |
| `documentation` | a `##` doc comment |

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
- **The package.** References and rename look through every `.plum` file
  under the nearest directory up with a `plum.toml` (hidden directories
  skipped), every open file and every file an open file imports. Without
  a `plum.toml`, the `.plum` files beside the file take the package's
  place. Each file is checked on its own, and two names are the same
  thing when they lead to the same declaration, so a local `count` in
  one function is never confused with another's.
- **Methods are renamed where they are declared.** A tree's method that
  overrides its parent's (or implements a trait's) is its own
  declaration: renaming one leaves the others as they are.
- **Positions** are in UTF-16 code units, the protocol's default.
- **Robustness.** Malformed JSON, a message without `Content-Length`,
  unknown methods (answered with `MethodNotFound`, `-32601`) and requests
  before `initialize` are answered with an error, and the server carries
  on. It exits on `exit`, with 0 after a `shutdown` request and 1 without
  one, or when its input ends.

## Editors

Where each editor shows the features beyond diagnostics, hover,
definition, completion, symbols and formatting (which all of them show):

| Feature | VS Code | Neovim (0.11) | Helix | Zed | Emacs (eglot) |
|---|---|---|---|---|---|
| Find references | Shift+F12 | `grr` | `gr` | Alt+Shift+F12 | `M-?` |
| Rename | F2 | `grn` | `Space r` | F2 | `M-x eglot-rename` |
| Signature help | as you type | `Ctrl-S` in insert mode | as you type | as you type | in the echo area (ElDoc) |
| Semantic tokens | with most themes (`editor.semanticHighlighting.enabled`) | on by default | not used (colours with tree-sitter) | only in versions that support them (otherwise tree-sitter) | `lsp-mode` with `lsp-semantic-tokens-enable`; eglot only in versions that support them |
| Code actions (quick fixes) | the light bulb, Ctrl+. | `gra` | `Space a` | Ctrl+. (Cmd+. on macOS) | `M-x eglot-code-actions` |

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
block-comment-tokens = { start = "#[", end = "]#" }
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

References and rename start from the declaration the name at the cursor
leads to: its file and the place of its name there. Every file of the
package is then checked (open files are already) and walked the same
way, and each name leading to that same place is a reference. A name
spelled differently can't be one, so most names are skipped without
looking them up.

Signature help lexes the text again (tokens skip strings and comments)
to find the innermost `(` still open before the cursor, the name before
it and the commas since; the call itself comes from the checked tree, or,
while it is still being typed, from checking the file once more with its
brackets closed at the cursor. Semantic tokens walk the checked tree as
the other answers do, and add the lexer's keywords, literals and
comments.

The tests are recorded sessions: `tests/lsp/*.in`, run by `tests/lsp.sh`
and compared with `tests/lsp/*.out`.
