# Testing: `plum test`

`plum test` runs the tests of a program or a package. Tests are ordinary
Plumlang: functions whose names start with `test_`, in files whose names
end in `_test.plum`, checking things with `assert`. The rules are in the
specification, [section 14.3](spec.md#143-testing).

## Writing tests

Put the tests for `shapes.plum` in `shapes_test.plum` next to it:

```plum
# shapes_test.plum
seed test
seed ./shapes { Circle, Square, parse_shape }

branch test_circle_area() {
    plant c = Circle(2.0)
    assert(c.area() > 12.56 and c.area() < 12.57, "area is {c.area()}")
}

branch test_square_sides_must_be_positive() {
    plant e = test.throws(() => {
        Square(-1.0)
    })
    assert(e is ValueError, "got a {type_of(e)}")
}
```

- A test is a top-level `branch test_name()`: no parameters, no result.
  Other functions in the file are helpers, not tests.
- A test passes when it returns and fails when an error escapes it. A
  failed `assert(condition, message)` throws an `AssertError` with the
  message; any other error (a `KeyError` from a missing map key, your own
  error tree) fails the test the same way.
- Give `assert` a message that shows the values: `"area is
  {c.area()}"` says more than `assertion failed`.

### Expected errors

To check that code throws, `seed test` and pass the code to
`test.throws` as a function. It returns the error, which `is` checks:

```plum
plant e = test.throws(() => {
    int("abc")
})
assert(e is ParseError)
assert(e.message.contains("abc"))
```

If the function throws nothing, `test.throws` fails the test with
`AssertError: expected an error, but none was thrown`, reported at the
line of your test that called it.

## Running them

```bash
plum test                           # every *_test.plum under the current directory
plum test examples/garden           # every one under a directory
plum test shapes_test.plum          # one file (its name may be anything)
plum test --filter circle           # only the tests whose name contains "circle"
plum test --json                    # a JSON report, for editors and CI
```

In a package (a directory with `plum.toml`), run `plum test` in that
directory; `seed ./...` in the tests finds the package's files the same
way the program does. Directories are searched in name order, and files
and directories starting with `.` are skipped.

## Each test on its own

Every test runs in a fresh program. The test file and the modules it
imports are loaded again and their top-level code runs, then the test
function is called. So:

- a module variable that one test changes is back to its first value in
  the next one, and objects, tasks and channels don't carry over;
- a test that fails, even with an uncaught error in a task or by calling
  `exit(1)`, doesn't stop the others;
- top-level code in a test file runs once per test: keep it to setup
  (constants, helper objects), and put checks in tests.

What lives outside the program is shared, between tests and with the rest
of your machine: files, the environment variables, the network. A test
that writes a file should use a name of its own and remove it.

## The report

```text
$ plum test
garden_test.plum
  ok   test_blooming_in_a_season
  FAIL test_tallest
       garden_test.plum:31: AssertError: assertion failed: a bed with plants has a tallest one
         in test_tallest, line 31
       output:
         beds: 1
  ok   test_season_names

2 passed, 1 failed
```

Each failure shows where the error was thrown (`file:line`), its tree and
message, the calls that led there, and anything the test printed. Output
from passing tests isn't shown. The exit code is 0 when every test passed
(or there were none), 1 when a test failed or a test file has compile
errors, 2 for a wrong option and 3 for a path that can't be read.

A test file with compile errors is reported like `plum check` reports
them, on standard error, and none of its tests run:

```text
math_test.plum
  FAIL it has errors, so its tests did not run
```

### JSON

`plum test --json` prints one JSON object on standard output (compile
errors still go to standard error):

```json
{
  "tests": [
    {"file": "math_test.plum", "name": "test_add", "line": 4, "passed": true},
    {"file": "math_test.plum", "name": "test_divide", "line": 8, "passed": false, "error": {"type": "AssertError", "message": "assertion failed: 7 // 2 is 3", "file": "math_test.plum", "line": 9, "trace": "  in test_divide, line 9\n"}, "output": ""}
  ],
  "broken": [],
  "passed": 1,
  "failed": 1
}
```

| Field | |
|---|---|
| `tests` | every test that ran, in order: its `file`, `name`, the `line` of its `branch`, and `passed` |
| `error` | for a failed test: the error's `type` (its tree) and `message`, the `file` and `line` it was thrown at, and the `trace` of calls. A test that failed without an error report (it called `exit(n)`) has only a `message`. |
| `output` | for a failed test: what it printed |
| `broken` | the test files that have compile errors |
| `passed`, `failed` | how many tests passed and failed |

## In CI

`plum test` exits 1 when anything fails, so a CI step is just the
command. With GitHub Actions and
[setup-plum](https://github.com/plumlang/setup-plum):

```yaml
- uses: plumlang/setup-plum@v1
- run: plum test
```
