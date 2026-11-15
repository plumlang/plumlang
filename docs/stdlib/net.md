# net

TCP and UDP sockets, TLS, and DNS. A program that serves many connections
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

## TLS

`net.tls_connect` is `connect` with TLS (1.2 or 1.3) on top; the
`Connection` it gives reads and writes like any other:

```plum
plant c = net.tls_connect("example.com", 443, timeout: 10.0)
c.write("HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n")
drop(c.read_line())
c.close()
```

The server's certificate is checked against the system's trusted CA
certificates and against the host name. The trusted CAs are, in order:
the file `SSL_CERT_FILE` names, if it is set; the system's bundle on
Linux and the BSDs (`/etc/ssl/certs/ca-certificates.crt`,
`/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`,
`/etc/ssl/cert.pem`) and macOS (`/etc/ssl/cert.pem`); the `ROOT` store on
Windows. They are read once, by the first connection that needs them.
`ca_file: "my-ca.pem"` trusts the CAs in that PEM file instead (a private
CA, a test server), and `verify: bad` checks nothing at all: the
connection is encrypted, but anyone in the middle can read it.

A TLS server takes its certificate chain and private key from PEM files:

```plum
plant server = net.listen_tls(8443, "cert.pem", "key.pem")
while good {
    plant conn = server.accept()
    spawn ((c: Connection) => serve(c))(conn)
}
```

`accept()` returns as soon as a client connects. The handshake is made by
the connection's first `read` or `write`, in the task that serves it, so
a client that is slow to finish it holds up no one else; a client whose
handshake fails shows up as a `NetError` from that `read` or `write`.

TLS is Mbed TLS 4.1 (`vendor/mbedtls`), built into `plum`. Handshakes
wait like everything else here: other tasks run while one does.

The handles are [`Listener`](net.listener.md),
[`Connection`](net.connection.md) and [`UdpSocket`](net.udp.md). They
close when the program closes them, or when nothing refers to them any
more.

## Waiting and time limits

Every socket is non-blocking underneath: `accept`, `connect`,
`tls_connect`, `read`, `read_line`, `write` and `receive` let other tasks
run while they wait. The time limit of `tls_connect` covers both the
connection and the handshake.
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
  that can't be looked up, a time limit running out, a closed handle. For
  TLS also a certificate that doesn't verify (`tls_connect to example.com
  port 443: certificate verify failed: the certificate is not correctly
  signed by the trusted CA`), a handshake that fails, and certificate or
  key files that can't be read.
- `ValueError`: a port outside 0 to 65,535 (1 to 65,535 to connect), a
  negative timeout, a `read` size outside 1 byte to 16 MB, a datagram
  over 65,507 bytes.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `listen` | `listen(port: int, host: str = "0.0.0.0") -> Listener` | a TCP listener on a port (0 picks a free one); listener.accept() takes connections |
| `connect` | `connect(host: str, port: int, timeout: float = 0) -> Connection` | a TCP connection to host and port |
| `tls_connect` | `tls_connect(host: str, port: int, timeout: float = 0, verify: bool = good, ca_file: str = "") -> Connection` | a TLS connection to host and port, its certificate checked against the system's trusted CAs (or ca_file) and the host name, unless verify is bad |
| `listen_tls` | `listen_tls(port: int, cert_file: str, key_file: str, host: str = "0.0.0.0") -> Listener` | a TLS listener, with a certificate chain and private key from PEM files; accept() takes connections, which make the handshake on their first read or write |
| `udp` | `udp(port: int = 0, host: str = "0.0.0.0") -> UdpSocket` | a UDP socket on a port (0 picks a free one) |
| `resolve` | `resolve(host: str) -> list[str]` | the addresses a host name has |
<!-- reference:end -->
