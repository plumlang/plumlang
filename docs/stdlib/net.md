# net

TCP and UDP sockets, and DNS. A program that serves many connections
gives each one a task (spec section 12): while a task waits for its
socket, the others run.

```plum
seed net

branch serve(conn: Connection) {
    plant line = conn.read_line()
    while line != none {
        conn.write("you said: {line ?? ""}\n")
        line = conn.read_line()
    }
}

plant server = net.listen(8080)
while good {
    plant conn = server.accept()
    spawn ((c: Connection) => serve(c))(conn)
}
```

A client:

```plum
plant c = net.connect("example.com", 80, timeout: 5.0)
c.write("HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n")
drop(c.read_line())
c.close()
```

The handles are [`Listener`](net.listener.md),
[`Connection`](net.connection.md) and [`UdpSocket`](net.udp.md). They
close when the program closes them, or when nothing refers to them any
more.

## Waiting and time limits

Every socket is non-blocking underneath: `accept`, `connect`, `read`,
`read_line`, `write` and `receive` let other tasks run while they wait.
Each takes a `timeout` in seconds (0, the default, means no limit) and
throws `NetError` with `...: timed out` when it runs out. Looking a name
up (`connect`, `resolve`, `send_to` with a host name) waits without
letting other tasks run.

The scheduler waits on sockets with epoll on Linux, kqueue on macOS and
the BSDs, and `WSAPoll` on Windows: idle connections cost nothing while
they wait. See `bench/net/idle.plum` and `bench/README.md` (10,000 idle
connections).

## Errors

- `NetError`: anything the network or the system refuses, with its
  reason (`connect to example.com port 81: Connection refused`), a name
  that can't be looked up, a time limit running out, a closed handle.
- `ValueError`: a port outside 0 to 65,535 (1 to 65,535 to connect), a
  negative timeout, a `read` size outside 1 byte to 16 MB, a datagram
  over 65,507 bytes.

TLS (https) isn't in the library yet.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `listen` | `listen(port: int, host: str = "0.0.0.0") -> Listener` | a TCP listener on a port (0 picks a free one); listener.accept() takes connections |
| `connect` | `connect(host: str, port: int, timeout: float = 0) -> Connection` | a TCP connection to host and port |
| `udp` | `udp(port: int = 0, host: str = "0.0.0.0") -> UdpSocket` | a UDP socket on a port (0 picks a free one) |
| `resolve` | `resolve(host: str) -> list[str]` | the addresses a host name has |
<!-- reference:end -->
