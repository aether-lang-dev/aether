- **`std.cryptography.tls13_client`: deadlines and cancellation end to end.**
  `connect`, `connect_mtls`, `connect_psk`, `connect_resume` and
  `connect_resume_0rtt` take `due_ms` and `cancel` (defaulted to none), and
  `conn_set_deadline(conn, due_ms, cancel)` rebinds them after the
  handshake; the socket variants, `connect_socket_psk` included, honour what
  the caller bound with `tcp.set_deadline`. One absolute deadline covers the
  dial, the handshake and every read and write, so a server that trickles a
  record a byte at a time is still cut off, and a cancel from another thread
  ends a blocked handshake, read or write at once. The error is exactly
  `tcp.DEADLINE_EXCEEDED` or `tcp.CANCELLED` whichever step it cut short,
  never an EOF or a TLS alert; the socket is closed once, by its owner, and
  secrets are wiped as on any failure. A stopped connection stays failed.
  The #3703 meaning of the socket's own receive timeout is unchanged.
