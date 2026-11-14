# Listener

The methods of `Listener` values from the [`net`](net.md) module. They
need no `seed` of their own.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `accept` | `accept(self: Listener, timeout: float = 0) -> Connection` | the next connection, waiting for one |
| `port` | `port(self: Listener) -> int` | the port it listens on |
| `close` | `close(self: Listener)` | stop listening |
<!-- reference:end -->
