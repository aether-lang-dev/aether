# Complete the pure-Aether TLS 1.3 **server**, so all four client/server × OpenSSL/pure permutations work

**Status of the tree when this was written:** PR #1804 adds `parse_client_hello`
and `server_hello` to `std/cryptography/tls13_hs/`. This document is the work
that remains after that lands.

**This is not about removing OpenSSL.** It stays the default where present.
The goal is that the pure path works on *both* ends, so our TLS gets exercised
against our TLS, and so a cross-built binary can serve HTTPS as well as consume
it.

---

## The permutation matrix (verified, not assumed)

|                | OpenSSL backend        | Pure-Aether backend                    |
| -------------- | ---------------------- | -------------------------------------- |
| **HTTP client** | ✅ works, native builds only | ✅ works, **including cross builds** |
| **HTTP server** | ✅ works, native builds only | ❌ **this task**                      |

Both "native only" cells were verified by cross-building and running:

```
$ ./cross_built_client            # std.http.client, --target=x86_64-linux
ERR: HTTPS requested but the build has no OpenSSL support (rebuild with OpenSSL installed)

$ ./cross_built_server            # std.http server_set_tls, same target
set_tls -> [TLS unavailable: built without OpenSSL]
```

Two things the empty cell costs:

1. **A cross-built binary cannot serve HTTPS at all.** Same hole the client side
   was filed about, from the other end.
2. **Our TLS is never tested against our TLS.** `tls13_client` is exercised by
   static RFC 8448 vectors and by OpenSSL peers only, so a mistake both halves
   make *together* has nowhere to surface.

---

## What already exists (do not rewrite any of this)

Measured before scoping, so the task is smaller than "implement a TLS server":

| Piece | Where | State |
| --- | --- | --- |
| Record layer `seal_record` / `open_record` | `std/cryptography/tls13_record/` | **role-neutral** — zero references to client/server; takes keys, not a role |
| Key schedule | `std/cryptography/tls13_ks/` | **role-neutral** — `traffic_secret(algo, secret, label, th, th_len)` is label-driven, so `"s hs traffic"` is the same call as `"c hs traffic"` |
| `finished_key` / `finished_verify_data` | `tls13_kdf` (exported ~line 124) | **role-neutral** — takes a secret |
| ECDSA / RSA-PSS signing | `p256.ecdsa_sign`, `rsa.sign_pss` | exists |
| `"TLS 1.3, server CertificateVerify"` context | `tls13_cert`, `cert_verify_content` | **already there**, used today for *verifying* |
| `cert_verify_content_ctx(context, hash, len)` | `tls13_cert` | parameterised by context string — a server signer is the same call with the server context |
| PEM parsing | `std/cryptography/pem/` `parse(text) -> (ptr, string, string)` | exists |
| Certificate parsing | `tls13_cert.parse_certificate` | exists |
| `parse_client_hello` / `server_hello` | `tls13_hs` (PR #1804) | **done** |
| Handshake message type constants | `tls13_hs` `HS_*` | all present |

---

## The work

### Task 1 — the four remaining server handshake messages

All in `std/cryptography/tls13_hs/module.ae`. Each is the **inverse of a parser
that already exists** in `std/cryptography/tls13_client/module.ae`, so read the
client's parser first and mirror it.

1. **`encrypted_extensions()`** — `HS_ENCRYPTED_EXTENSIONS = 8`. In the minimal
   case this is an empty extensions block (2 zero bytes) wrapped in a handshake
   header. The client already parses it.
2. **`certificate_msg(chain_der, lengths)`** — `HS_CERTIFICATE = 11`. RFC 8446
   §4.4.2: 1-byte `certificate_request_context` (empty for a server), then
   `certificate_list<0..2^24-1>` of `{ cert_data<1..2^24-1>, extensions<0..2^16-1> }`.
   Extensions are empty per entry in the minimal case.
3. **`certificate_verify_msg(scheme, signature)`** — `HS_CERTIFICATE_VERIFY = 15`.
   `{ algorithm(2), signature<0..2^16-1> }`. The signature is over
   `cert_verify_content(transcript_hash)` — the **server** context string, which
   already exists. Add `sign_server_cert_verify_ecdsa_p256` in `tls13_cert`
   mirroring the existing `sign_client_cert_verify_ecdsa_p256` (line ~110);
   the only difference is which `cert_verify_content*` it hashes.
4. **`finished_msg(verify_data, len)`** — `HS_FINISHED = 20`. Body is just the
   verify_data. Compute it with the existing `finished_key` /
   `finished_verify_data` from the **server** handshake traffic secret.

**Follow the existing style exactly**: `put8`/`put16`/`put24`/`put_bytes` helpers,
a generous fixed buffer, patch lengths at the end, copy to a right-sized buffer,
free the scratch. `server_hello` (PR #1804) is the worked example.

### Task 2 — a `tls13_server` module driving the flight

New: `std/cryptography/tls13_server/module.ae`, mirroring
`std/cryptography/tls13_client/module.ae` (2308 lines — read it, this is the
same shape reversed).

```
accept(sock, cert_chain, cert_lens, priv_key, x25519_priv) -> (ptr, string)
conn_send(conn, data, len) -> string        // same signature as the client's
conn_recv(conn) -> (ptr, int, string)
close_conn(conn)
```

Flight order (RFC 8446 §2), the mirror of `run_handshake_tail` (client line ~924):

1. read ClientHello → `parse_client_hello`
2. X25519 shared secret from the client's key_share + our ephemeral private
3. key schedule: early → handshake → master, deriving **both** directions
4. send ServerHello (plaintext), then under handshake keys:
   EncryptedExtensions, Certificate, CertificateVerify, Finished
5. read the client's Finished, verify its MAC
6. switch to application traffic keys

### Task 3 — wire it into the HTTP server

`std/net/aether_http_server.c`, function **`conn_tls_accept` (line ~695)**, which
today calls `SSL_accept`. Add a pure branch selected the same way the client
side will be — an env/option, defaulting to OpenSSL where compiled in.

`conn_recv` / `conn_send` (lines ~305 and ~316) **already dispatch on `c->ssl`**,
so the framing layer above needs no changes. This is the same C/Aether boundary
the client has, from the other side; look at how PR #1804's vertical-slice test
drives the client from Aether for the shape.

---

## Testing — the bar this must clear

Follow `tests/integration/crypto_tls13_server_hs/` and
`tests/integration/https_pure_tls_vertical/` (both from PR #1804).

**Required, in increasing order of what they prove:**

1. Each new message builder round-trips through the **client's existing parser**
   for that message. Those parsers were written independently against RFC 8448,
   so agreement is not a builder checking its own output.
2. The vertical-slice test extended to **all four permutations**: pure client ↔
   pure server, pure client ↔ OpenSSL server, OpenSSL client (curl) ↔ pure
   server, and the existing OpenSSL ↔ OpenSSL.
3. **`curl --cacert ... https://127.0.0.1:port/` against the pure server must
   work.** This is the one that matters. Everything above passes if our two
   halves agree on a wire format nobody else speaks; curl is a third-party
   implementation and will reject a malformed flight.
4. Negative cases: malformed ClientHello, unsupported group, bad Finished MAC.

**Do not** mark this done on 1 and 2 alone.

---

## Pitfalls, all of which have already cost time in this tree

- **`ptr` means a `std.bytes` HANDLE, never `bytes.data(b)`.** Aether has no
  separate type, so both spell as `ptr`; passing the raw data pointer **compiles
  cleanly and segfaults** somewhere unrelated. `conn_recv` likewise *returns* a
  handle — read it with `bytes.to_string(p, n)`, not `bytes.string_from_ptr`.
  This cost five failed attempts to call shipped APIs correctly with the source
  open. See the header of `tls13_client/module.ae`.
- **Adding a field to `LeafCert`**: use the exported `LEAFCERT_SIZE` constant and
  the exported `init_leaf`. Before PR #1804 there were three hand-counted
  `malloc(136)` calls and two duplicate initialisers; adding one pointer
  corrupted the heap and surfaced as a double-free far from the edit.
- **`tests/ae_sweep_prune.txt`**: any new test directory whose `.ae` files are
  not standalone programs (a client needing a peer, a server that blocks) must
  be listed there, or the bulk sweep runs them and CI fails while local shell
  tests pass.
- **`fmt_gate` covers `std/` as well as `tests/`.** Run
  `ae fmt std examples tests` before pushing; `make test` does not cover it.
- **HelloRetryRequest is not implemented** and `parse_client_hello` returns an
  explicit error for it. A browser or a client preferring a group we do not
  support will hit that path. Implementing HRR is optional for a first cut but
  must be *stated*, not discovered.

## Definition of done

`curl` completes an HTTPS request against an Aether server built with **no
OpenSSL at all**, and the same server is reachable from `tls13_client`. Both
run in CI, and a cross-built (`ae build --target=...`) server binary serves
HTTPS.
