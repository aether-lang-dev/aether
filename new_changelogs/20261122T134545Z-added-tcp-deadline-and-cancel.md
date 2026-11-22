- **`std.tcp`: one deadline per connection, and a cancel handle.**
  `tcp.connect_until(host, port, due_ms, cancel)` and
  `tcp.set_deadline(sock, due_ms, cancel)` bind an absolute deadline on the
  monotonic clock and a cancel handle (`tcp.cancel_new`, `tcp.cancel`,
  `tcp.cancelled`, `tcp.cancel_retain`, `tcp.cancel_free`) to a connection: the dial and every
  read and write after end by the deadline however slowly the peer trickles,
  and a cancel from any thread wakes a blocked call at once. They end with
  `tcp.DEADLINE_EXCEEDED` or `tcp.CANCELLED`, distinct from `"timeout"` and
  from a close, and both are sticky. Neither closes anything: the cancel
  writes to its own descriptor (eventfd, pipe, or a loopback pair on
  Windows), polled beside the socket, and the owner closes, so a cancel
  racing a close cannot close a reused descriptor. The handle is counted:
  each other thread that may trip it takes its own reference with
  `tcp.cancel_retain` and drops it with `tcp.cancel_free`. Per connection, passed
  explicitly; see `docs/design/tcp-tls-deadlines-and-cancellation.md`.
