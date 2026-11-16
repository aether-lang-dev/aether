- **std.http.client: an https request through the pure-Aether TLS client
  gives up on a silent server at the request's timeout (#3705).** The
  client replaced the timeouts the request had put on the socket with
  std.tcp's 30 s and retried a timed-out handshake read 50 times, so a
  server that accepted the connection and never answered the ClientHello
  held the request for about 25 minutes whatever its `set_timeout`. The
  client now takes the descriptor with the request's timeouts (std.net's
  `tcp_socket_adopt_fd`) and ends the handshake on a timed-out read, with
  the error "no reply from the server within the timeout", as the OpenSSL
  client already did. A request without a timeout waits as the OpenSSL
  client does, and reads after the handshake are unchanged. Covered by
  `tests/integration/https_client_handshake_timeout`, through each backend
  the build has.
