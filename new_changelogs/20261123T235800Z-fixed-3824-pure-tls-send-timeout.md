- **A pure-Aether TLS write that timed out is not written again either.**
  The pure TLS client reported a write that waited out the socket's send
  timeout as any other failure, so the HTTP client took it for an I/O
  error and wrote a POST whose head timed out with nothing counted as sent
  again on a fresh connection. A std.tcp socket now records whether its
  last send that waits for room ended by its send timeout
  (`tcp_send_timed_out_raw` in std/net/aether_net.h), the pure client
  reports such a write as a timeout, and the HTTP client's one rule for
  timed-out writes refuses to send it again (#3824).
