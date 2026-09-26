- **`std.udp`: datagram sockets for game networking (#2201).**
  `std.tcp` had the reliable half; a game's state snapshots run on datagrams,
  because one that arrives late is worth less than none and must not wait
  behind a lost one the way TCP's in-order stream makes it. ae3d had to keep
  a C file of its own for that. Now `udp.bind(host, port)` (port 0 for an
  ephemeral one, read back with `local_port`; IPv4 and IPv6, dual-stack on
  `"::"` where the OS allows) gives a socket that is non-blocking from the
  start: `recv_from(sock, buf, cap)` returns `(n, sender, err)` and reports
  `"would block"` when nothing is waiting, so a loop drains it once a tick and
  never waits; `poll(sock, timeout_ms)` and `fd` are there for the program
  that does. A sender is an address value: `addr_equal`, `addr_hash`,
  `addr_string` (`host:port`), `addr_clone`, and `send_to_addr` answer it
  without resolving anything, and `resolve(host, port)` makes one up front for
  a server answering many peers a tick; `recv_from_into` fills a caller's
  address so the drain loop allocates nothing. `send_to(sock, host, port,
  data, n)` resolves per call. `set_buffer_sizes` sets `SO_SNDBUF`/`SO_RCVBUF`,
  `set_broadcast` allows LAN discovery. The C layer is held to numbers in
  `tests/runtime/test_runtime_udp.c` (loopback, truncation, zero-length
  datagrams, address semantics, IPv6) and the wrappers in
  `std/udp/test_udp.ae`.
