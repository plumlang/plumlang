# UdpSocket

The methods of `UdpSocket` values from the [`net`](net.md) module. They
need no `seed` of their own.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `send_to` | `send_to(self: UdpSocket, host: str, port: int, text: str)` | send one datagram |
| `receive` | `receive(self: UdpSocket, max: int = 65536, timeout: float = 0) -> str` | the next datagram, waiting for one |
| `sender` | `sender(self: UdpSocket) -> str` | where the last datagram received came from, as "host:port" |
| `port` | `port(self: UdpSocket) -> int` | the port it is on |
| `close` | `close(self: UdpSocket)` | close the socket |
<!-- reference:end -->
