- **std.http.client: a request's timeout means the same on both TLS
  backends (#3706).** Through the pure-Aether client, a response that did
  not come within the request's `set_timeout` was read again up to 50 times
  (`RECIO_MAX_TIMEOUT_RETRIES`): a 500 ms timeout waited 25 s for a server
  that sent nothing, and returned a reply that came at 4 s, where the
  OpenSSL client fails at the first timeout. The retries date from when a
  descriptor could arrive non-blocking and a brief pause read as a timeout
  at once; the client's socket is now blocking with the caller's timeouts,
  so a timed-out read has waited the whole timeout and ends the wait. The
  connection survives it: `tls13_client.conn_recv` returns "timeout" (the
  new `EV_TIMEOUT` event), and a caller of the module that wants an idle
  stream may wait again. std.tcp now takes a receive that a signal
  interrupted again instead of reporting it as "timeout", so the sentinel
  means the socket's timeout. Covered by
  `tests/integration/https_client_response_timeout`, through each backend
  the build has.
