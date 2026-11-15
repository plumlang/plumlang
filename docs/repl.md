# The REPL

`plum` with no arguments, on a terminal, starts an interactive prompt: type
Plumlang, press Enter, and it runs at once. `plum repl` does the same and
also reads piped input. The rules are in [spec section 14.2](spec.md#142-the-interactive-prompt).

```
$ plum
Plumlang 0.0.1. Type :help for help, :quit to leave.
plum> plant x = 20
plum> x * 2 + 2
= 42 : int
plum> branch greet(name: str) -> str {
  ...     return "hello, {name}"
  ... }
plum> greet("ada")
= hello, ada : str
plum> :type greet
(str) -> str
plum> seed math
plum> math.sqrt(2.0)
= 1.4142135623730951 : float
plum> x // 0
<repl>:1: ZeroDivisionError: division by zero: 20 // 0
  in <main>, line 1
plum> x
= 20 : int
plum> :quit
```

## Entries

An entry is a line, or several: it goes on to the next line (with the
`...` prompt) while a bracket is open, a line ends with an operator, `,`,
`=` or `=>`, or a `"""` string is open. Ctrl-C drops the entry being typed.

- What an entry declares stays for the entries after it: variables,
  functions, trees, traits, kinds and `seed` imports. Declaring a name
  again hides the old one from then on.
- An entry that is one expression prints its value, as `drop` would, and
  its type: `= [1, 2] : list[int]`. A `none` value prints nothing.
- A compile error shows the usual diagnostic, pointing into the entry. A
  runtime error shows its message and where it happened. Either way the
  session goes on with what earlier entries made; the failed entry leaves
  nothing it declared behind.
- `seed ./file` is relative to the directory `plum` was started in. A file
  is loaded and run once, by the first entry that imports it.
- Tasks run while an entry runs. The entry ends when its own code does;
  tasks still running carry on while later entries run, so
  `plant t = spawn work()` in one entry and `t.wait()` in the next works.
- `exit(n)` ends the session with exit code `n`.

## Commands

| Command | |
|---|---|
| `:help` | The commands and keys. |
| `:type expr` | The type of an expression, without running it. |
| `:load file.plum` | Run a file's code as an entry; its declarations stay. |
| `:reset` | Forget everything declared so far. |
| `:quit`, `:q` | Leave (so do Ctrl-D on an empty line and `exit()`). |

## Keys

| Key | |
|---|---|
| Left, Right (Ctrl-B, Ctrl-F) | Move the cursor. |
| Home, End (Ctrl-A, Ctrl-E) | To the start or end of the line. |
| Backspace, Delete | Delete before or under the cursor. |
| Up, Down (Ctrl-P, Ctrl-N) | Earlier lines. |
| Ctrl-U, Ctrl-K | Delete to the start or end of the line. |
| Ctrl-W | Delete the word before the cursor. |
| Ctrl-L | Clear the screen. |
| Ctrl-C | Drop the entry being typed. |
| Ctrl-D | Leave, on an empty line; otherwise delete under the cursor. |

The history keeps the last 1000 lines in `.plum_history` in the home
directory (`$HOME`, or `%USERPROFILE%` on Windows). Set `PLUM_HISTORY` to
another file, or to nothing to keep no history file.

The line editor is built in: raw mode through termios on Linux and macOS,
the console's key events on Windows. Without a terminal (input from a pipe
or a file), lines are read as they come, with no prompts, which is how the
tests in `tests/repl/` drive it. Ctrl-C while an entry runs stops `plum`.
