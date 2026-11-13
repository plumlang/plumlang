# math

```plum
seed math

drop(math.sqrt(2.0))                 # 1.4142135623730951
drop(math.max(3, 9), math.abs(-2.5)) # 9 2.5
drop(math.round(3.14159, digits: 2)) # 3.14
drop(math.random_int(1, 6))          # a die roll
```

Angles are in radians. Functions that take a `float` also take an `int`.

## Errors

- `ValueError`: `sqrt` of a negative number; `log` of 0 or less, or with a
  base of 0 or less or 1; `asin` / `acos` outside -1 to 1; `clamp` or
  `random_int` with `low` above `high`; `round` with more than 15 digits
  either way.
- `OverflowError`: `abs(MIN_INT)`; `floor` / `ceil` of a float too large
  for an int, or of NAN.

## Random numbers

Each program starts its random numbers from a seed the operating system
picks, so runs differ. `math.seed_random(n)` restarts them from `n`: the
same seed gives the same numbers, on every platform (the generator is
xoshiro256**, seeded with splitmix64). They are not for cryptography.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `abs` | `abs(x: num) -> num` | the value without its sign |
| `min` | `min(a: num, b: num) -> num` | the smaller of two numbers |
| `max` | `max(a: num, b: num) -> num` | the larger of two numbers |
| `clamp` | `clamp(x: num, low: num, high: num) -> num` | x kept within low to high |
| `floor` | `floor(x: float) -> int` | the int at or below x |
| `ceil` | `ceil(x: float) -> int` | the int at or above x |
| `round` | `round(x: float, digits: int = 0) -> float` | x rounded to that many decimals; halves go away from zero |
| `sqrt` | `sqrt(x: float) -> float` | the square root |
| `exp` | `exp(x: float) -> float` | e to the power x |
| `log` | `log(x: float, base: float = E) -> float` | the logarithm, natural by default |
| `sin` | `sin(x: float) -> float` | sine of an angle in radians |
| `cos` | `cos(x: float) -> float` | cosine of an angle in radians |
| `tan` | `tan(x: float) -> float` | tangent of an angle in radians |
| `asin` | `asin(x: float) -> float` | the angle whose sine is x |
| `acos` | `acos(x: float) -> float` | the angle whose cosine is x |
| `atan` | `atan(x: float) -> float` | the angle whose tangent is x |
| `atan2` | `atan2(y: float, x: float) -> float` | the angle of the point (x, y) |
| `is_nan` | `is_nan(x: float) -> bool` | is x not a number (NAN)? |
| `is_finite` | `is_finite(x: float) -> bool` | is x neither infinite nor NAN? |
| `random` | `random() -> float` | a random float from 0 up to (not including) 1 |
| `random_int` | `random_int(low: int, high: int) -> int` | a random int from low to high, both included |
| `seed_random` | `seed_random(value: int)` | restart the random numbers, so they repeat |
| `PI` | `PI: float` | the ratio of a circle's circumference to its diameter |
| `TAU` | `TAU: float` | 2 * PI, a full turn in radians |
| `E` | `E: float` | the base of natural logarithms |
| `INF` | `INF: float` | positive infinity |
| `MAX_INT` | `MAX_INT: int` | the largest int |
| `MIN_INT` | `MIN_INT: int` | the smallest int |
<!-- reference:end -->
