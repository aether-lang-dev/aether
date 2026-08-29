# tls13_client tests

`test_tls13_client.ae` — the **automated** test (runs in CI). Validates the
pure, offline pieces of `std.cryptography.tls13_client` against the canonical
RFC 8448 §3 trace: the transcript-hash accumulator, the handshake key
derivation, and the Finished key/verify-data. No network.

`../https_pure_tls_vertical/` — the **automated end-to-end** test. Stands up
our own `std.http` server with TLS and drives a full socket handshake against
it from this client, then frames an HTTP/1.1 exchange over the encrypted
stream. That covers the socket path the KAT above deliberately does not: a
real ServerHello, a real certificate chain, hostname verification (by IP, via
an `iPAddress` SAN) and application data both ways. No OpenSSL on the client
side, which is what makes it the check that a cross build still has working
HTTPS.

`../crypto_tls13_server_hs/` — the server-direction handshake messages,
including parsing a **real ClientHello captured from `openssl s_client`**, so
the wire format is validated against another implementation rather than only
against ourselves.

## Live-server handshake check against OpenSSL (manual)

The two automated tests above cover our own peers. This reproduces a handshake
against OpenSSL's `s_server` specifically, which is worth doing by hand when
touching the handshake: it is the check that catches a mistake both of our
halves make together. Kept manual rather than in CI because a live-server
integration test is inherently flakier than a KAT. To reproduce:

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
| P-384 leaf + multi-cert chain | `en.wikipedia.org` (ECDSA-P384 leaf, leaf→2 intermediates→root) | **CONNECTED** — P-384 CertificateVerify verified, full chain built to a system anchor |
| P-256 leaf + multi-cert chain | `github.com` (leaf→2 intermediates→root) | **CONNECTED** — chain built to a system anchor |
| untrusted | CA-signed, CA **not** in trust store | rejected — does not chain to a trusted anchor |
| self-signed | self-signed, `SAN=localhost` | rejected — does not chain to a trusted anchor |
| wrong host | CA-signed, `SAN=example.com`, connect as `localhost` | rejected — not valid for the requested hostname |
| expired | CA-signed, `notAfter=2020` | rejected — certificate expired |
| mTLS optional (`s_server -verify 1`) | server requests a client cert | **CONNECTED** — client sends an empty Certificate (declines), server proceeds |
| mTLS required (`s_server -Verify 1`) | server *requires* a client cert | rejected — server alerts "peer did not return a certificate"; client surfaces it as `tls alert` |

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

**Remaining limits:** server CertificateVerify is wired for ECDSA-P256,
ECDSA-P384, and RSA-PSS-rsae-SHA256 (Ed25519 / P-521 leaf CertVerify fail
closed); the cert *chain* verify handles RSA + ECDSA (P-256/P-384) and builds a
full leaf→intermediate→…→root path against the system trust store; OCSP is
staple-only (no CRL / no OCSP fetch), responder signature not verified. See the
module header.
