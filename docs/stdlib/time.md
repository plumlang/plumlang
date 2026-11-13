# time

Clocks, sleeping and dates. Times are `float` seconds since
1970-01-01 00:00 UTC, as `time.now()` gives them.

```plum
seed time

plant start = time.clock()
time.sleep(0.5)
drop("took {time.clock() - start:.2} s")

drop(time.format(time.now()))                       # 2026-11-13 21:04:05
drop(time.format(time.now(), "%d %B %Y", utc: good)) # 13 November 2026
drop(time.parts(time.now())["weekday"])              # 1 Monday … 7 Sunday
```

`clock` is for measuring how long something takes: it never goes back,
even when the time of day is changed. `now` is the time of day.

Dates are local time unless `utc: good` is given. `format` knows these
patterns, the same on every platform:

| Pattern | Gives | Pattern | Gives |
|---|---|---|---|
| `%Y` | year, 2026 | `%y` | year, 26 |
| `%m` | month, 01 to 12 | `%B` / `%b` | month name, November / Nov |
| `%d` | day, 01 to 31 | `%A` / `%a` | weekday name, Friday / Fri |
| `%H` | hour, 00 to 23 | `%p` | AM or PM |
| `%M` | minute | `%S` | second |
| `%j` | day of the year, 001 to 366 | `%Z` | time zone name |
| `%%` | a `%` | | |

## Errors

- `ValueError`: `sleep` with a negative time (or more than 10,000,000
  seconds); `format` with a pattern not listed above; `format` / `parts`
  of a time outside the years 1 to 9999.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `now` | `now() -> float` | the time of day: seconds since 1970-01-01 00:00 UTC |
| `clock` | `clock() -> float` | seconds on a clock that never goes back, for measuring how long things take |
| `sleep` | `sleep(seconds: float)` | wait that many seconds |
| `format` | `format(t: float, pattern: str = "%Y-%m-%d %H:%M:%S", utc: bool = bad) -> str` | a time as text; local time unless utc |
| `parts` | `parts(t: float, utc: bool = bad) -> map[str, int]` | year, month, day, hour, minute, second, weekday (1 Monday to 7 Sunday) and yearday |
<!-- reference:end -->
