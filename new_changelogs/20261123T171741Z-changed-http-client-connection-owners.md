- **HTTP client connections belong to an owner, so a connection one caller
  authenticated never serves another.** `std.http.client` kept one idle
  pool for the whole process, keyed by origin, and NTLM and Negotiate
  authenticate the connection: a recording proxy (servirtium-vcr) sent an
  anonymous client's request on a connection another client had logged in,
  and it was answered 200 as that user. Reuse now goes through a pool the
  program makes and passes on each request: `client.pool_new()`,
  `client.set_pool(req, pool)`, `client.pool_free(pool)`. A request with no
  pool, which includes the one-line `http.get`, `post`, `put` and `delete`,
  has a connection of its own, closed after its response. A pinned pool
  (`client.pool_new_pinned()`) keeps a sequence of requests on one
  connection, and the server's new `http.request_conn_id(req)` and
  `http.server_on_conn_close(server, cb, ud)` let a proxy pair each client
  connection with an upstream connection and let it go when the client
  closes. Breaking: `http.client_pool_configure`, `client_pool_disable`,
  `client_pool_clear`, `client_pool_idle_count` and
  `client_rx_buffer_allocs` are removed, replaced by `client.pool_configure`,
  `pool_clear`, `pool_idle_count` and `pool_rx_buffer_allocs` on a pool; a
  loop of requests with no pool now pays a handshake each. A
  `std.http.proxy` pool owns its upstream connections
  (`proxy.pool_conns(pool)`), and the C `http_client_pool_*_raw` and
  `http_upstream_acquire*` functions take the pool. See
  `docs/design/http-client-connection-owners.md`.
