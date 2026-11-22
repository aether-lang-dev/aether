- **`std.cryptography.tls13_client`: TLS 1.3 with an external PSK.**
  `tls13_client.connect_psk(host, port, identity, key)` and
  `connect_socket_psk(sock, identity, key)` run a handshake authenticated by
  a key the two ends were given out of band (RFC 8446 2.2), for a device
  with no certificate path, such as a smart plug and its phone controller:
  a 32-byte SHA-256 key and a 1 to 64-byte identity (any bytes, NUL
  included), psk_dhe_ke only, so every session has a fresh ECDHE secret and
  a psk_ke answer is refused. The binder is keyed with "ext binder", never
  the resumption's "res binder" (new `tls13_ks.external_binder_key`), and is
  computed again after a HelloRetryRequest; the ClientHello offers the two
  SHA-256 suites alone (`tls13_hs.client_hello_psk` takes `only_sha256`).
  A server that does not take the PSK, sends a Certificate, selects another
  identity or answers in TLS 1.2 is refused: there is no certificate
  fallback. The key is copied and the copy wiped on every path; the socket
  handed to `connect_socket_psk` is the call's from then on, closed by it on
  every failure. Tested against `openssl s_server -psk`, Python 3.13's
  `set_psk_server_callback` and a scripted peer that misbehaves on purpose.
