# Connection

The methods of `Connection` values from the [`net`](net.md) module. They
need no `seed` of their own.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `read` | `read(self: Connection, max: int = 65536, timeout: float = 0) -> str?` | what has arrived (waiting for something), up to max bytes; none once the other end closes |
| `read_line` | `read_line(self: Connection, timeout: float = 0) -> str?` | the next line, without its line break; none at the end |
| `write` | `write(self: Connection, text: str, timeout: float = 0)` | send all of text, waiting for room as needed |
| `remote` | `remote(self: Connection) -> str` | the other end, as "host:port" |
| `port` | `port(self: Connection) -> int` | this end's port |
| `close` | `close(self: Connection)` | close the connection |
<!-- reference:end -->
