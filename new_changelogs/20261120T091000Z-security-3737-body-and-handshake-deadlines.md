- **Request bodies and TLS handshakes have budgets, and the head's deadline
  holds through TLS (#3737).** The per-read idle timeout bounds one wait
  for a byte, so a client that sent a byte just inside it, over and over,
  held a server worker for as long as it liked. Now:
  - a TLS handshake must finish within 30 s, on both TLS backends
    (`http.server_set_tls_handshake_timeout`), or the connection closes;
  - the server waits on a client for a request body no longer than 30 s
    plus one second for every 500 bytes received
    (`http.server_set_body_timeout(server, 30s, 500)`), counting only the
    time it waits, so a slow handler does not use up the client's budget.
    Past it the answer is 408 and the connection closes, whether the server
    reads the body before the handler (a chunked body, a small one) or the
    handler reads it with `request_body` or `request_body_read`;
  - the head's 30 s deadline from its first byte now also cuts off a TLS
    record that arrives a byte at a time, and, on a server with HTTP/2 on,
    counts the bytes read to look for the HTTP/2 preface, which were read
    with no deadline at all.

  A body that stopped arriving for the idle timeout while the handler read
  it used to end short with no sign: the handler answered the cut body as if
  it were whole, 200 included. It is now answered 408 too.
