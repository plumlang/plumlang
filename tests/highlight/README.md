# Highlighting sample

One source of truth for the grammars that highlight Plumlang in editors
and on the web: [tree-sitter-plum](https://github.com/plumlang/tree-sitter-plum)
(the reference), the VS Code grammar in
[branding](https://github.com/plumlang/branding/tree/main/vscode),
[zed-plum](https://github.com/plumlang/zed-plum),
[highlightjs-plum](https://github.com/plumlang/highlightjs-plum) and
[pygments-plum](https://github.com/plumlang/pygments-plum).

| File | What |
|---|---|
| `keywords.txt` | Every keyword, one per line: the lexer's table in `src/lexer/lexer.c`, printed by `tools/keywords.sh`. |
| `sample.plum` | Every keyword, every literal form, interpolation with formats and nested braces, `"""` strings, `#` in strings and comments, nested `#[ ... ]#` block comments (one spanning lines), `##` doc comments, `#[` and `##` inside strings, every operator. `plum check` accepts it. |

`tests/highlight.sh` (part of `make test`) fails when the list and the
lexer disagree, or when the sample misses a keyword or a kind of literal.
Each grammar's CI fetches both files from `main`:

```
https://raw.githubusercontent.com/plumlang/plumlang/main/tests/highlight/keywords.txt
https://raw.githubusercontent.com/plumlang/plumlang/main/tests/highlight/sample.plum
```

and fails when a keyword is missing from the grammar or the sample doesn't
highlight cleanly. So adding a keyword means: add it to the lexer, run
`sh tools/keywords.sh > tests/highlight/keywords.txt`, use it in the sample,
then update each grammar.
