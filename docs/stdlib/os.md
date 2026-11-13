# os

The program's arguments, the environment, and running commands. To end the
program, use the built-in `exit(code)`.

```plum
seed os

plant args = os.args()            # plum run greet.plum Ada → ["Ada"]
plant name = if len(args) > 0 { args[0] } else { "friend" }
drop("Hello, {name}! ({os.platform()})")
plant home = os.env("HOME") ?? "unknown"
```

`os.run` runs a command through the system's shell (`sh` on Linux and
macOS, `cmd` on Windows) and gives its exit code; a command ended by a
signal gives 128 plus the signal's number. Never build a command from
text a user typed: the shell would run whatever it says.

## Errors

- `IOError`: `run` when the shell can't be started; `cwd` when the
  working directory can't be read.
- `ValueError`: `run` with a command containing a zero byte.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `args` | `args() -> list[str]` | the arguments after the program's file: plum run app.plum a b gives ["a", "b"] |
| `env` | `env(name: str) -> str?` | an environment variable, or none if it isn't set |
| `platform` | `platform() -> str` | "linux", "macos", "windows" or "unix" |
| `cwd` | `cwd() -> str` | the working directory |
| `run` | `run(command: str) -> int` | run a command through the system's shell and give its exit code |
<!-- reference:end -->
