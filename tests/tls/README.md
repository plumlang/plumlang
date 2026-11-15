# Test-only TLS certificates

**For the tests only. Never trust these anywhere.** The private key is
here in the open, so anyone can impersonate this server.

| File | What |
|---|---|
| `ca.pem` | a CA certificate, "Plumlang test CA (not trusted anywhere)" |
| `server.pem` | a server certificate for `localhost` and `127.0.0.1`, signed by that CA |
| `server.key` | the server's private key (EC P-256, PKCS#8, unencrypted) |

Both certificates are valid for 100 years from when they were made, so
the tests don't start failing as they age. The CA's own key was thrown
away once `server.pem` was signed. The tests (`tests/lang/tls/`) serve
`server.pem` on loopback and trust it with `ca_file: "tests/tls/ca.pem"`;
no test uses the network beyond the machine itself.

They were made with OpenSSL by [`make-certs.sh`](make-certs.sh):

```sh
sh tests/tls/make-certs.sh
```

Running it again makes new keys and certificates; commit all three files
together.
