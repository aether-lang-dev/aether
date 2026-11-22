# Reply: TLS 1.3 external-PSK client

Answers `tls13-external-psk-client.md`. Shipped in one PR with the TLS
deadlines and task execution contexts (branch
`batch/nov22-esp32-psk-deadlines-task-contexts`, not yet merged or released;
the release that carries it is the one to pin).

## The API

In `std.cryptography.tls13_client`:

```text
connect_psk(host: string, port: int, identity: byte[], key: byte[],
            due_ms: long = 0, cancel: ptr = null) -> (ptr, string)
connect_socket_psk(sock: @keep ptr, identity: byte[], key: byte[]) -> (ptr, string)
```

Both return the module's usual `(ClientConn, "")` or `(null, why)`;
`conn_send`, `conn_recv`, `conn_key_update`, `conn_set_deadline` and
`close_conn` work on the connection as on any other. `due_ms` and `cancel`
are the deadline and cancel handle of
`REPLY-tls-client-absolute-deadlines-and-cancellation.md`;
`connect_socket_psk` takes whatever the caller bound to the socket with
`tcp.set_deadline`. Also new: `tls13_ks.external_binder_key(algo, early)`,
and `tls13_hs.client_hello_psk` takes `only_sha256`.

## The contract

- A 32-byte key (SHA-256) and an identity of 1 to 64 bytes, both `byte[]`,
  any bytes, NUL included. Anything else is refused before a byte is sent.
  These are the ask's bounds; nothing found argued for wider ones.
- psk_dhe_ke only: the server must answer with a key share (x25519, or
  secp256r1 after a HelloRetryRequest), so every session has a fresh ECDHE
  secret. A psk_ke answer (no key_share) is refused, missing_extension.
- The binder is keyed with "ext binder" (RFC 8446 7.1), never "res binder",
  over the ClientHello truncated before the binders, and is computed again
  after a HelloRetryRequest over message_hash(ClientHello1), the
  HelloRetryRequest and the second ClientHello.
- Only TLS_CHACHA20_POLY1305_SHA256 and TLS_AES_128_GCM_SHA256 are offered;
  TLS_AES_256_GCM_SHA384 in a ServerHello or a HelloRetryRequest is
  illegal_parameter.
- The server must select identity 0, the only one offered
  (illegal_parameter otherwise).
- No certificate fallback: a server that does not take the PSK is refused
  (handshake_failure), as is a Certificate in the flight after taking it
  (unexpected_message), and TLS 1.2 (protocol_version).
- A wrong key cannot complete. A server that checks the binder refuses it
  (decrypt_error from OpenSSL, mbedTLS refuses too); one that did not check
  derives other handshake keys, so its flight does not open ("authentication
  failed"); a Finished that does not verify is refused, compared in constant
  time. Each is an error string distinct from EOF, "timeout",
  `tcp.DEADLINE_EXCEEDED` and `tcp.CANCELLED`.
- No 0-RTT, no early data. `connect_psk` sends `host` as SNI;
  `connect_socket_psk` sends none.

## Ownership and secrets

- `identity` and `key` are borrowed for the call and never kept. The key is
  copied into the handshake's own buffer, which is wiped on success and on
  every failure path, as is every secret derived from it (early, binder,
  handshake and Finished keys, the ECDHE secret, the private scalars). The
  caller still owns and wipes its own copy.
- The connection keeps the application traffic secrets and the resumption
  master secret, like any other; `close_conn` wipes them.
- The socket handed to `connect_socket_psk` is the call's from then on. The
  connection closes it at `close_conn`; every failure, a refused argument
  included, closes it before returning. The caller never closes it after the
  call. A null socket is refused (nothing to close).
- The ephemeral shares, client random and session id come from the system
  CSPRNG; if it fails the call fails closed before sending anything.

## Tested against

`tests/integration/crypto_tls13_external_psk/test_external_psk.sh`:

- `openssl s_server -psk -psk_identity -nocert -tls1_3` (independent):
  host and adopted socket; `-groups P-256` (a HelloRetryRequest) on both;
  ChaCha20; wrong key; wrong identity; SHA-384 only (refused, the client
  never offers it); a certificate server with no PSK (refused); TLS 1.2
  (refused). 10 cases.
- Python `ssl` with `set_psk_server_callback` (3.13 or later, `ssl.HAS_PSK`;
  independent): host, socket, P-256 retry on both, wrong key, wrong
  identity, certificate server, TLS 1.2 PSK. 8 cases; skipped, saying so,
  where no such Python exists.
- `pskpeer.ae`, a scripted server computing its own secrets, for what no
  real server does on purpose: selected_identity 1, psk_ke, SHA-384 in a
  ServerHello and in a HelloRetryRequest, no pre_shared_key, a Certificate
  after taking the PSK, a bad Finished, a TLS 1.2 ServerHello, a wrong key
  past a server that skips the binder check, a 64-byte identity, one with
  NUL bytes, and which label keyed the binder. 16 cases.
- Socket ownership seen from the peer (`ownclient.ae`, `sink.py`): each
  refused argument and a refused handshake closes the socket before the call
  returns.
- Mutations shown to fail the tests: the external binder keyed with "res
  binder" (22 cases fail, OpenSSL and Python report "binder does not
  verify"); the close dropped on a refused argument (the ownership case
  fails). The existing resumption and 0-RTT suite (`crypto_tls13_resume`)
  passes unchanged.

## Where it ran (2026-11-22)

- macOS arm64 (this Mac): the TLS/TCP/HTTP/crypto selection of `make test-ae`
  294 pass, 0 fail, 5 skip; `make test` 763/763. All three peers (OpenSSL
  4.1.0, Python 3.13 with OpenSSL 3.5.9, scripted).
- cachyos (gcc 16): the same selection 293/0/6 (skips: websockets module,
  Windows junctions, the allocator for crypto_key_wipe); Python 3.14 and
  OpenSSL 4.1.0 peers. ASan with LeakSanitizer and UBSan over the new and
  core TLS/TCP tests: 38 pass, 0 fail, 1 skip each, and `make test` 765/765
  under each. `crypto_random_failure` (a failing CSPRNG) now covers
  `connect_psk` too: its first draw failing refuses it.
- ghostbsd (FreeBSD 15.1): the selection 295/0/4; this test alone 38 cases
  (OpenSSL 3.5.6; Python skipped, 3.12); `crypto_key_wipe`'s new case (the
  key copy and derived secrets of a handshake stopped by its deadline) finds
  no copy in freed memory.
- ae-x64 (Ubuntu 22.04, gcc 11, -Werror): `make ci` passes, 765 C unit
  tests, 2321 pass / 0 fail / 28 skip.
- Windows (x86_64 cross-build, Wine 9): the plain and HelloRetryRequest
  handshakes against the scripted peer.
- ESP32 under Espressif QEMU (cachyos, the esp32 repo's `psk-probe`
  firmware, mbedTLS, the lane's disposable fixture key): `connect_psk` and
  `connect_socket_psk` get `HTTP/1.1 200` from `/status`; a wrong key is
  refused (decrypt_error) and an unknown identity too (the firmware does not
  take the PSK); the firmware's own OpenSSL checks pass around it.

## Not done

- Keys other than 32-byte SHA-256 ones, SHA-384 PSKs, more than one
  identity, external-PSK 0-RTT, and the RFC 9258 importer interface (an
  imported PSK is derived with its own labels and is not conflated with this
  raw external PSK).
- SNI is sent as given even for an IP literal, as the certificate path does
  (RFC 6066 says not to); left alone here for consistency, worth a separate
  fix across both paths.
