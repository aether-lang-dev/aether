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
    handler reads it with `request_body` or `request_body_read`. The rate
    is held over a sliding window, 60 s unless
    `http.server_set_body_rate_window` sets another: what the budget has
    left never grows past it through bytes received, so a client that sends
    a burst and then trickles below the rate is cut off within the window,
    where a 60 MiB burst had earned 35 hours. The same holds on HTTP/2
    streams and on the proxy event loop;
  - the head's 30 s deadline from its first byte now also cuts off a TLS
    record that arrives a byte at a time, and, on a server with HTTP/2 on,
    counts the bytes read to look for the HTTP/2 preface, which were read
    with no deadline at all. Records that carry no data (a TLS 1.3
    KeyUpdate, on either TLS backend) do not start it: a kept-alive
    connection is still idle after one.

  A body that stopped arriving for the idle timeout while the handler read
  it used to end short with no sign: the handler answered the cut body as if
  it were whole, 200 included. It is now answered 408 too.
  A wait the deadline bounds that runs out is the deadline passing, read
  so whatever the millisecond clock says a moment later: a poll may return
  a fraction of a millisecond early (macOS), and a TLS head cut there was
  taken for an idle connection and closed with no 408.
