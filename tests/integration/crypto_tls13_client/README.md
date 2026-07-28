# tls13_client tests

`test_tls13_client.ae` — the **automated** test (runs in CI). Validates the
pure, offline pieces of `std.cryptography.tls13_client` against the canonical
RFC 8448 §3 trace: the transcript-hash accumulator, the handshake key
derivation, and the Finished key/verify-data. No network.

## Live-server handshake check (manual)

The full socket-driven handshake is verified against a real TLS 1.3 server
(OpenSSL `s_server`) rather than in CI, because a live-server integration test
is inherently flakier than a KAT. To reproduce:

```sh
# 1. a P-256 self-signed cert (exercises the ECDSA CertificateVerify path)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout server.key -out server.crt -days 2 -nodes -subj "/CN=localhost"

# 2. a TLS 1.3 server: ChaCha20-Poly1305, X25519, HTTP responder
openssl s_server -accept 4433 -tls1_3 \
  -ciphersuites TLS_CHACHA20_POLY1305_SHA256 -groups X25519 \
  -cert server.crt -key server.key -www -quiet &

# 3. an Aether program that calls tls13_client.connect("127.0.0.1", 4433, priv)
#    then conn_send(GET) / conn_recv — it prints the decrypted "HTTP/1.0 200 ok".
```

A successful run: `connect()` completes with the server Finished MAC verified,
and `conn_recv` returns the decrypted HTTP response.

**Caveat:** this cut does NOT authenticate the server — no CertificateVerify
signature check, no chain/hostname validation. It proves the key exchange and
record protection work end-to-end against a real server; it is not yet safe
against an active MITM. See the module header.
