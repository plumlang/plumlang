# fs

Files and directories. Text is read and written as it is (UTF-8, no
newline translation), and paths use `/` on every platform (Windows accepts
it too).

```plum
seed fs

fs.write("notes.txt", "first line\n")
fs.append("notes.txt", "second line\n")
for line in fs.read("notes.txt").lines() {
    drop(line)
}

if not fs.exists("out") {
    fs.make_dir("out")
}
drop(fs.list("."))          # the names here, sorted
```

## Errors

Every function that touches the disk throws `IOError` when the operating
system refuses, with its reason: a missing file, no permission, a
directory that isn't empty, and so on. `exists` and `is_dir` never throw.
An empty path, or one containing a zero byte, is an `IOError` too.

```plum
try {
    plant config = fs.read("config.txt")
} catch e: IOError {
    drop("no config: {e.message}")
}
```

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `read` | `read(path: str) -> str` | the whole file as text |
| `write` | `write(path: str, text: str)` | make or replace the file with text |
| `append` | `append(path: str, text: str)` | add text to the end of the file |
| `exists` | `exists(path: str) -> bool` | is there a file or directory at path? |
| `is_dir` | `is_dir(path: str) -> bool` | is path a directory? |
| `remove` | `remove(path: str)` | delete a file, or an empty directory |
| `rename` | `rename(from: str, to: str)` | move or rename a file or directory |
| `make_dir` | `make_dir(path: str)` | make a directory (fine if it already exists) |
| `list` | `list(path: str) -> list[str]` | the names in a directory, sorted |
| `temp_dir` | `temp_dir() -> str` | a directory for temporary files |
<!-- reference:end -->
