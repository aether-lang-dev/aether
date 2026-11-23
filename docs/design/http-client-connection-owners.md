# HTTP client connections belong to an owner

Status: shipped in the release after 0.938.0. Code: `std/net/aether_http.c`
(the pool), `std/http/client/module.ae` (the API), `std/net/aether_http_server.c`
(connection ids and the close callback). Tests:
`tests/integration/http_client_conn_owners/`, and
`http_pinned_pool_lends_one_connection` and `http_pool_shared_by_threads` in
`tests/runtime/test_runtime_http.c`.

## The bug

`std.http.client` kept one idle-connection pool per process, keyed by
origin (and the proxy dialled, TLS and verification settings). Any request
to an origin could take any idle connection to it. That is fine for a
connection that carries nothing but bytes, and wrong for one that carries
identity: NTLM and Negotiate authenticate the CONNECTION. After the
handshake, every request on that connection is the user's, with no
credential on it.

servirtium-vcr's NTLM harness (`pre-recorded/ntlm/` in
`servirtium/servirtium-vcr`, its README's finding (a), and its
`asks/aether-http-client-connection-affinity.md`) showed what that does to a
recording proxy, which forwards requests for many clients through one
client library:

- an anonymous request from a second client is answered 200 as the user the
  first client logged in;
- two clients interleaving handshakes share one upstream connection, and the
  second Type 1 spoils the first's Type 3;
- a Type 3 sent on a new client connection succeeds through the proxy, where
  the server refuses it directly;
- the leak crosses two independent recorders in one process.

Turning the pool off stops the leak and makes connection-bound auth
impossible to forward at all: every leg on a new connection.

## What changed

There is no process-wide pool. A pool is a value with an owner.

```aether,fragment
pool, err = client.pool_new()          // up to 64 idle, 8 per origin, 15 s
client.set_pool(req, pool)             // take an idle connection, give it back after
client.pool_free(pool)                 // closes everything it holds
```

- **A request with no pool** opens a connection of its own, sends
  `Connection: close`, and closes it after the response. That covers the
  one-line `http.get` / `post` / `put` / `delete`, which have no owner to
  share with. It is correct and safe by construction: nothing a request
  does to a connection outlives the request.
- **A pool** (`pool_new`) lends its idle connections to the requests sent
  through it and to no others. Within it, connections are keyed as before
  (origin, proxy, TLS backend, verification), and everything the old pool did
  stays: a connection is kept only after a response with definite framing
  that did not ask to close; a stale idle connection is redialled once; the
  receive buffer travels with the connection (#2160); expiry is lazy
  (#1719). Two pools never share a connection.
- **A pinned pool** (`pool_new_pinned`) holds at most one connection and
  never expires it for idleness. A sequence of requests through it rides one
  connection for as long as the server keeps it open. It lends that
  connection to one request at a time: a request sent while another is in
  flight goes out on a connection of its own, closed after its response, so
  the pinned connection is never displaced. A response that closes the
  connection drops it, and the next request dials again, exactly as the
  server would see a direct client do.
- **The proxy** (`std.http.proxy`) owns a pool per upstream pool
  (`AetherProxyPool`), sized for a proxy when the pool is made, shared by the
  mounts bound to it and by both proxy drivers (the worker path and the event
  loop). `proxy.pool_conns(pool)` lends it out for inspection and clearing.
  Health probes have no owner and so dial per probe.
- **The server** gives each accepted connection an id
  (`http.request_conn_id(req)`, 1, 2, ... per server, never reused) and calls
  the program back when one closes (`http.server_on_conn_close`). A proxy
  that must mirror client connections upstream keeps a pinned pool per
  client connection id and frees it in the close callback.

## Why this shape

- **Instances own their state.** The pool is a struct the caller creates and
  frees, passed explicitly on each request, the same way `tcp.cancel_new`
  hands out a cancel handle and `guest` hands out a session. The alternative
  the ask sketched, a connection value set on a request, is the pinned pool
  here; making it a pool of one keeps one type, one lifetime rule and one set
  of verbs (`pool_clear` is the ask's `connection_close`).
- **No keyed global.** A process-wide map from "owner key" to pool would be
  the same shared state with a longer key, and a library inside a program
  could still be handed connections the program made. An owner value cannot
  be guessed.
- **The default is the safe one.** Pooling across calls by default is what
  leaked. A program that wants reuse says so by making a pool, and reuse
  within it measures the same as before (below). A program that makes many
  unpooled requests in a loop pays a TCP (and TLS) handshake for each, which
  is the visible cost of this change.
- **No compatibility shims.** `http.client_pool_configure`,
  `client_pool_disable`, `client_pool_clear`, `client_pool_idle_count` and
  `client_rx_buffer_allocs` are gone; their per-pool forms are
  `client.pool_configure`, `pool_clear`, `pool_idle_count` and
  `pool_rx_buffer_allocs`. The C side's `http_client_pool_*_raw` functions
  take the pool, and `http_upstream_acquire*` take the pool the connection is
  for.

## Threads

A pool may be used from several threads at once: its list and settings are
under its own lock (a pthread mutex; the context lock in a cooperative,
`AETHER_NO_THREADING` build, where task contexts can interleave), and a
connection is out of the list while a request holds it, so no two requests
ever share one. A pinned pool's in-flight flag is an atomic. `pool_free`
must not race a send through the same pool: free it after the last request
through it has returned. `http_pool_shared_by_threads` (four threads, one
pool, 160 requests) runs under ThreadSanitizer in `make test-tsan`.

## Cost

A loop of 20,000 GETs to a loopback keep-alive server, macOS arm64, the same
program built against 0.938.0 (its process-wide pool) and against this change
(one owner's pool): 234 to 240 ms before, 221 to 223 ms after, over three
runs. The same requests with no pool: about 43 microseconds each against 11,
the handshake per request.
