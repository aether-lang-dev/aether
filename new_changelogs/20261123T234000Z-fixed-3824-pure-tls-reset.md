- **A reset under the pure-Aether TLS client is reported as a reset.** Over
  a plain socket and over OpenSSL, a read the upstream's reset ended was
  "connection reset by the peer before the response arrived", which the
  reverse proxy answers 502; under the pure-Aether TLS client every failure
  that was not a timeout was "I/O error". A std.tcp socket now records
  whether its last receive failed because the peer reset or aborted the
  connection, read from the error that receive left (`tcp_reset_raw` in
  std/net/aether_net.h), and the pure client reports such a read to the
  HTTP client as a reset, never by an errno something later may have set
  (#3824).
