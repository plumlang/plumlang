# Task and Channel

The methods of tasks and channels (spec section 12). They need no `seed`.

```plum
seed time

branch fetch(n: int) -> int {
    time.sleep(0.1)        # other tasks run meanwhile
    return n * 10
}

plant jobs = [spawn fetch(1), spawn fetch(2), spawn fetch(3)]
for j in jobs {
    drop(j.wait())         # 10, 20, 30: after about 0.1 s in all, not 0.3 s
}
```

`wait()` and `receive()` can't be called inside a callback (`map`,
`filter`, `to_str`, ...), since a task can only wait when nothing else is
running; use a `for` loop, as above.

A channel carries values from task to task. `for x in channel` receives
until the channel is closed and empty. `Channel[T]()` has no limit;
`Channel[T](capacity: n)` makes `send` wait while `n` values are waiting
to be received.

```plum
plant results = Channel[str]()
spawn (() => {
    for name in ["a", "b", "c"] {
        results.send(name.upper())
    }
    results.close()
})()
for r in results {
    drop(r)
}
```

## How tasks run

All tasks run on one thread, taking turns: a task runs until it waits (for
a channel, another task, or `time.sleep`), and then the next one in line
runs. `time.sleep(0)` gives the others a turn. So two tasks never run at
the same moment, and nothing needs locking. A task costs a few kilobytes,
so thousands of them are fine.

The program ends when its own code ends; tasks still running then are
stopped. A task that fails is not lost: `wait()` throws its error, and an
error nothing waited for is printed when the program ends (exit code 1).
If every task is waiting and nothing can wake them, that's a deadlock,
also reported, with exit code 1.

## Errors

- `wait()` throws the task's error, with the trace of where it happened.
- `Error`: `send()` on a closed channel; `wait()`, `receive()` or a full
  `send()` inside a callback; a task waiting for itself.
- `ValueError`: a negative channel capacity.

## Reference

The signatures use `T` for the task's result or the channel's values.

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `wait` | `wait(self: Task[T]) -> T` | the task's result once it ends; if it failed, its error is thrown here |
| `done` | `done(self: Task[T]) -> bool` | has the task ended? |
| `new_channel` | `new_channel(capacity: int = 0) -> Channel[T]` | Channel[T](capacity): a channel; a capacity of 0 means no limit |
| `send` | `send(self: Channel[T], value: T)` | put a value in the channel; waits for room if it has a capacity and is full |
| `receive` | `receive(self: Channel[T]) -> T?` | the next value, waiting for one; none once the channel is closed and empty |
| `close` | `close(self: Channel[T])` | no more values: receivers get what's left, then none |
<!-- reference:end -->
