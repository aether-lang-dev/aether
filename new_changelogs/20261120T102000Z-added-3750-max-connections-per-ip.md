- **A per-address connection limit for the HTTP server (#3750).**
  `http.server_set_max_connections_per_ip(server, n, ipv6_prefix)` bounds
  the connections one client address may hold, the worker path and the
  proxy event loop counted together; past it a new connection is answered
  503 and closed (closed alone over TLS), its close lingering no more than
  500 ms so the answer is not reset away. It is off by default: behind a
  load balancer or a NAT every client arrives from one address, and any
  default limit would refuse real traffic there, as nginx's opt-in
  `limit_conn` reflects. An IPv6 client counts by its first `ipv6_prefix`
  bits, 64 by default, since one host usually holds a whole /64.
