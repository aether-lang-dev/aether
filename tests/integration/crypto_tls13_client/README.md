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

A P-256 server cert is required (`-newkey ec -pkeyopt
ec_paramgen_curve:prime256v1`), because the client verifies the server's
CertificateVerify signature and only the ECDSA-P256 scheme is wired so far.

## Server authentication (Nic's bar)

`connect()` now fails closed unless the server cert passes all of: the
CertificateVerify signature check, the validity window, hostname/SAN match, and
a chain to a trusted anchor (the system CA bundle via `SSL_CERT_FILE` or the OS
default). This was verified end-to-end against `openssl s_server`:

| Case | Server cert | Result |
|------|-------------|--------|
| valid + trusted + `SAN=localhost` | CA-signed, CA in trust store | **CONNECTED** |
| untrusted | CA-signed, CA **not** in trust store | rejected — does not chain to a trusted anchor |
| self-signed | self-signed, `SAN=localhost` | rejected — does not chain to a trusted anchor |
| wrong host | CA-signed, `SAN=example.com`, connect as `localhost` | rejected — not valid for the requested hostname |
| expired | CA-signed, `notAfter=2020` | rejected — certificate expired |

Reproduce a rejection, e.g. self-signed:

```sh
openssl ecparam -name prime256v1 -genkey -noout -out ss.key
openssl req -x509 -new -key ss.key -out ss.crt -days 2 -sha256 \
  -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost"
openssl s_server -accept 4433 -tls1_3 \
  -ciphersuites TLS_CHACHA20_POLY1305_SHA256 -groups X25519 \
  -cert ss.crt -key ss.key -www -quiet &
# a client program that calls connect("localhost", 4433, priv) prints the
# rejection: "certificate does not chain to a trusted anchor"
```

**Remaining limits:** only ECDSA-P256 CertificateVerify is wired (an RSA server
CertificateVerify fails closed; the cert *chain* verify handles RSA + ECDSA);
the leaf must chain directly to a trusted anchor (no intermediate-CA path
building yet); no revocation (CRL/OCSP). See the module header.
