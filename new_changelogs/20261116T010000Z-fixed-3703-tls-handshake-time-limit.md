- **std.net: the TLS handshake has a time limit (#3703).** A client that
  sent the first bytes of a ClientHello and stopped held a server worker (a
  pool thread, or an actor-mode worker's core) for as long as it kept the
  connection open: `http_server_drain_connection` shook hands before it put
  the receive timeout on the socket, and the pure-Aether TLS server then
  replaced the socket's timeouts with std.tcp's 30 s and retried a timed-out
  handshake read 500 times. The handshake now runs under the server's
  keep-alive idle timeout (30 s unless set), the bound a request already
  had: each read or write that makes no progress for that long ends the
  handshake and the connection closes, as a stalled request head does. On
  both backends: OpenSSL's, and the pure-Aether server's, which takes the
  descriptor with the server's timeouts (std.net's new `tcp_socket_adopt_fd`)
  and ends its handshake on the socket's own timeout. A client within the
  timeout shakes hands as before, and the timeouts after the handshake are
  unchanged. The event driver bounded its own handshake already. Covered by
  `tests/integration/http_tls_handshake_timeout`.
