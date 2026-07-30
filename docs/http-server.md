# `std.http` HTTP Server

Aether's built-in HTTP server. Routes, request/response, middleware
chain, TLS termination, HTTP/1.1 keep-alive, per-connection actor
dispatch, HTTP/2 (h2 + h2c via libnghttp2 with ALPN, GOAWAY graceful
shutdown, per-stream concurrent dispatch via a server-level pthread
pool), WebSocket (RFC 6455), Server-Sent Events, structured access
logs, Prometheus metrics, graceful shutdown, health probes, all in
`std.http` and `std.http.middleware`.

---

## Quickstart

```aether
import std.http

handle_root(req: ptr, res: ptr, ud: ptr) {
    http.response_set_status(res, 200)
    http.response_set_header(res, "Content-Type", "text/plain")
    http.response_set_body(res, "hello")
}

main() {
    server = http.server_create(8080)
    http.server_get(server, "/", handle_root, 0)
    http.server_start_raw(server)  // blocks
}
```

Run it: `ae run server.ae`. Hit `http://127.0.0.1:8080/` from any client.

---

## Routing

```aether
http.server_get   (server, "/users",      handle_list,   0)
http.server_get   (server, "/users/:id",  handle_get,    0)
http.server_post  (server, "/users",      handle_create, 0)
http.server_put   (server, "/users/:id",  handle_update, 0)
http.server_delete(server, "/users/:id",  handle_delete, 0)
http.server_add_route(server, "PATCH", "/users/:id", handle_patch, 0)
```

Path parameters (`:id`) are extracted and reachable via
`http.get_path_param(req, "id")`. Query parameters via
`http.get_query_param(req, "key")`.

---

## Static files

```aether
http.server_get(server, "/report", handle_report, 0)

handle_report(req: ptr, res: ptr, ud: ptr) {
    http.serve_file(res, "/srv/reports/latest.pdf")
}

// Or serve a whole directory:
handle_assets(req: ptr, res: ptr, ud: ptr) {
    http.serve_static(req, res, "/srv/www")
}
```

`http.serve_file(res, filepath)` streams one file with `Content-Type`
resolved via `http.mime_type(filepath)`. Under HTTP/1.1 cleartext on
Linux/macOS it takes a zero-copy `sendfile(2)` fast path (the body
never touches the heap; `TCP_CORK`/`TCP_NOPUSH` coalesces headers and
body into one segment). TLS, HTTP/2, Range requests, and Windows fall
back to a buffered read.

`http.serve_static(req, res, base_dir)` dispatches the request path
against a directory. Path traversal (`..`, `%2e` encodings) is
rejected with 403, missing files return 404, and `Range` headers are
honoured for partial content.

---

## TLS termination

```aether
err = http.server_set_tls(server, "/etc/ssl/cert.pem", "/etc/ssl/key.pem")
if err != "" { println("TLS setup: ${err}"); exit(1) }
```

After this call every accepted connection completes a TLS handshake
(`TLS 1.2+`, compression and renegotiation disabled) before the HTTP
parse runs. Plaintext clients hitting the TLS port get rejected at
handshake time. The cert and key must be PEM-encoded; the server
validates they match.

When the build does not include OpenSSL (`AETHER_HAS_OPENSSL`
undefined), `server_set_tls` returns `"TLS unavailable: built without
OpenSSL"`.

---

## HTTP/2 (h2 + h2c)

`std.http` speaks HTTP/2 server-side via libnghttp2 (server-side HTTP/2
Tier 2). Enable with one call alongside the existing TLS / keep-
alive setup:

```aether
import std.http

main() {
    server = http.server_create(443)
    http.server_set_tls(server, "/etc/ssl/cert.pem", "/etc/ssl/key.pem")
    http.server_set_h2(server, 0)        // 0 = nghttp2 default 100
    http.server_set_keepalive(server, 1, 0, 30000ms)

    http.server_get(server, "/", home, 0)
    http.server_start(server)
}
```

What the toggle changes:

- **ALPN**, the TLS handshake now advertises `h2` first and
  `http/1.1` as the fallback. Modern clients (curl `--http2`,
  Chrome, Firefox, Envoy, fan-out gateways) negotiate HTTP/2;
  older clients keep working on HTTP/1.1 transparently.
- **h2c upgrade**, plain (non-TLS) connections now honour
  `Upgrade: h2c` + `HTTP2-Settings` headers per RFC 7540 §3.2.
- **Prior-knowledge h2**, clients that send the HTTP/2
  connection preface immediately (e.g. `curl
  --http2-prior-knowledge`) on a plain socket are auto-detected
  and switched to HTTP/2 without an HTTP/1.1 round trip.
- **Stream demux**, every h2 stream dispatches into the same
  route table as HTTP/1.1, so all middleware (gzip / cors / auth
  / ratelimit / vhost / rewrite / static), all metrics + access
  logs, and all health probes apply uniformly.

`max_concurrent_streams` is the SETTINGS_MAX_CONCURRENT_STREAMS
value advertised to peers. Pass `0` for libnghttp2's default
(100).

### Per-stream concurrent dispatch

By default, h2 streams within one connection dispatch sequentially
on the connection thread, each handler runs to completion before
the next stream's handler begins. For workloads where handlers do
non-trivial work (database queries, large I/O, downstream HTTP
calls), this caps the throughput a single TCP connection can drive.

`http.server_set_h2_concurrent_dispatch(server, n)` routes stream
handlers onto the **shared `std.worker` pool**, sized to `n`
threads. Handlers run on that pool in parallel; the connection
thread keeps reading frames and serialising responses. One
process-wide pool serves both `worker.run` and every h2 connection
on every server. POSIX-only (macOS / Linux); on Windows the call is
silently ignored and streams stay sequential.

```aether
http.server_set_h2(server, 0)                          // h2 enabled
http.server_set_h2_concurrent_dispatch(server, 4)      // 4-way fan-out
```

Sizing guidance:

| `n` | When to use |
|----|-------------|
| `0` (default) | Handlers are CPU-light or already async. Sequential dispatch keeps memory + thread count low. |
| `4`  | Most servers, handlers do a DB hit / upstream call per request. |
| `8`–`16` | Heavy fan-out, handlers block on slow I/O. |
| `> 16` | Diminishing returns; bound by physical cores and the connection accept queue. The setter caps at 64. |

#### Why pthreads, not actors?

Aether actors are **virtual**, N actors are M:N-scheduled over a
small pool of OS threads. They're ideal for non-blocking,
cooperative work: spawning 10,000 actors costs near-zero memory
and switches happen in user-space. They're *wrong* for HTTP
handlers, though: HTTP handlers in this stack call arbitrary
blocking C/Aether code (`sleep`, `fs.read`, `db.query`,
`http.client_get`). A blocking actor monopolises its scheduler
thread until it returns. With the actor scheduler holding only a
few worker threads, blocking handlers under load would starve the
host's entire actor system, including unrelated actors that have
nothing to do with HTTP.

Pool threads sidestep that: each blocked handler ties up *one* OS
thread but the kernel keeps the rest of the system responsive. The
OS scheduler is the right primitive when work units may block.

#### Why one shared pool, not per-connection?

Dispatch runs on the shared `std.worker` pool, one process-wide set
of reusable threads. With a per-connection pool of size 4 and 1,000
keep-alive clients, the process would carry 4,000 pthreads, most of
them idle. A single shared pool keeps the OS thread count bounded by
`n` regardless of connection (or server) fan-out, mirroring how
`HttpConnectionPool` (the existing HTTP/1.1 worker pool) is sized
once at server startup. Reusing `std.worker` also means the whole
runtime draws blocking work from one budget instead of standing up a
second, redundant h2-private pool.

Per-session state (the wake pipe + ready queue) stays local
because the connection thread doing nghttp2 serialisation is
per-connection, targeted wake-up means a worker finishing a task
on connection A doesn't bother connection B's poll loop.

#### Lifetime + correctness

- nghttp2_session is **not** thread-safe, only the connection
  thread calls into it. Workers receive the (request, response)
  pair, run the route handler, and post the result back via the
  session's ready queue. The connection thread submits the
  response to nghttp2 on its own thread.
- Each session tracks an `in_flight` counter. `aether_h2_session_free`
  spins on it until all worker tasks belonging to that session
  have been submitted, so the session never goes away while a
  worker is mid-handler. This per-session spin, not a pool join, is
  what guarantees no task touches freed server state: sessions are
  freed before the server, and each session's tasks have completed
  by the time it is freed.
- The shared `std.worker` pool outlives any single server (it is
  process-lifetime), so `http_server_free` frees no pool. Call
  `worker.pool_shutdown()` for deterministic teardown when every
  job is known to have finished.
- Graceful shutdown (`http_server_shutdown_graceful`) waits for
  in-flight tasks to complete before flipping `want_close`,
  workers can't be discarded mid-handler.

**Smoke test:**

```bash
curl --http2-prior-knowledge http://localhost:8080/
curl --http2 https://localhost:443/        # ALPN-negotiated
```

`curl -w '%{http_version}'` prints `2` when HTTP/2 was
negotiated.

**When libnghttp2 isn't linked**, `http.server_set_h2` returns
`"HTTP/2 unavailable: built without libnghttp2"`. The HTTP/1.1
path keeps working unchanged. Build-time detection is automatic
via `pkg-config libnghttp2`; install the package to opt in:

| OS | Package |
|---|---|
| Debian/Ubuntu | `apt install libnghttp2-dev` |
| Fedora/RHEL | `dnf install libnghttp2-devel` |
| macOS (Homebrew) | `brew install nghttp2` |
| MSYS2/MinGW64 | `pacman -S mingw-w64-x86_64-nghttp2` |

**Middleware + transformer compatibility.** Every middleware /
response-transformer / hook / probe in `std.http` works
identically on h2 streams as on HTTP/1.1 requests because all
three protocols (`http_server_dispatch_for_h2` plus the original
HTTP/1.1 path) feed the same chain:

| Surface | h2 streams |
|--------|------------|
| `middleware.use_cors`         | ✓ |
| `middleware.use_basic_auth`   | ✓ |
| `middleware.use_bearer_auth`  | ✓, RFC 6750 challenges (`error="invalid_token"` for malformed credentials) emitted regardless of protocol |
| `middleware.use_session_auth` | ✓, `Cookie:` header parsed identically on h2 streams; redirect-on-failure works |
| `middleware.use_rate_limit`   | ✓ |
| `middleware.use_vhost`        | ✓ |
| `middleware.use_static_files` | ✓ |
| `middleware.use_rewrite`      | ✓ |
| `middleware.use_real_ip`      | ✓, X-Real-IP appended to the request, visible to handlers + downstream middleware on every h2 stream |
| `middleware.use_gzip` (response transformer) | ✓, `Content-Encoding: gzip` rides on the h2 HEADERS frame |
| `middleware.use_error_pages`  | ✓ |
| `http_server_set_health_probes` | ✓ |
| `http_server_set_metrics`     | ✓ |
| `http_server_set_access_log`  | ✓, every h2 stream is one log entry |

WebSocket and SSE remain HTTP/1.1-only, they use protocol-
specific upgrade paths that don't apply to h2. Clients that need
real-time pushes over h2 should use h2 server-streamed responses
(periodic DATA frames) instead, or open an h1 connection
specifically for WS/SSE alongside the h2 traffic.

**Per-protocol response semantics:**

- `HEAD` requests on an h2 stream auto-fall through to the
  matching `GET` handler so `Content-Length` and headers
  describe what `GET` would return; the wrapper suppresses the
  DATA frames per RFC 7231 §4.3.2.
- Connection-specific request headers (`Connection`,
  `Transfer-Encoding`, `Upgrade`, `Keep-Alive`,
  `Proxy-Connection`) are rejected with PROTOCOL_ERROR on h2
  streams (RFC 7540 §8.1.2.2). The same names are stripped from
  responses on h2 streams, so handlers that emit them for
  HTTP/1.1 don't need to be h2-aware.
- Pseudo-header presence (`:method`, `:scheme`, `:path`) is
  validated; missing or empty `:path` reaches RST_STREAM with
  PROTOCOL_ERROR rather than the route table.
- The `:path` pseudo-header is split on `?` to populate
  `req->path` + `req->query_string` separately, matching the
  HTTP/1.1 parser shape.

**Trade-offs:**

- HTTP/2 streams within one connection dispatch *sequentially* by
  default; opt into per-stream parallelism with
  `http.server_set_h2_concurrent_dispatch(server, n)`, which
  provisions a server-level pthread pool of `n` workers shared
  across every h2 connection on the server. Stream handlers run
  on the pool while the connection thread keeps reading frames
  and serialising responses. nghttp2_session is not thread-safe,
  only the connection thread calls into it; the pool wakes the
  connection thread via a self-pipe whose read end is exposed as
  `aether_h2_session_wake_fd` for the caller to poll alongside
  its socket fd. POSIX-only; on Windows the call is a silent
  no-op and dispatch stays sequential.
- HPACK Huffman encoding is enabled by default on POSIX
  (`NGHTTP2_NV_FLAG_NONE` leaves the choice to nghttp2 per RFC
  7541 §5.2). libnghttp2 decodes Huffman-encoded headers from
  clients regardless of platform.
- Server-push (PUSH_PROMISE, RFC 7540 §6.6) is not implemented;
  rarely used in practice and tracked as an optional follow-up.
- The h2 wrapper holds one nghttp2 session per connection. Memory
  cost is bounded by `SETTINGS_HEADER_TABLE_SIZE` (default 4 KiB)
  + per-stream state. With 100 concurrent streams the upper
  bound is ~64 KiB per connection.

**Troubleshooting.**

- *"HTTP/2 unavailable: built without libnghttp2"*, install
  `libnghttp2-dev` (or distro equivalent) and rebuild. Auto-
  detected via `pkg-config libnghttp2`.
- *Curl reports `http_version=1.1` even though h2 is enabled*
  the client either isn't asking for h2 (`curl --http2` or
  `--http2-prior-knowledge`) or is using TLS without ALPN
  support. Check `curl --version` mentions `nghttp2` in the
  feature list.
- *Curl `--http2` over plain HTTP shows `1.1`*, this is
  curl's "passive upgrade", it falls back if the upgrade
  isn't probed. `--http2-prior-knowledge` forces h2 from the
  first byte.
- *PROTOCOL_ERROR on the wire*, the server rejected a
  malformed request (most likely a forbidden connection-
  specific header per RFC 7540 §8.1.2.2). Check the access
  log for the offending stream.
- *h2c upgrade succeeds but no h2 data follows*, keep-alive
  must be enabled (`http.server_set_keepalive(server, 1, …)`)
  for the connection to survive past the upgrade response.

---

## HTTP/1.1 keep-alive

```aether
http.server_set_keepalive(server, 1, 100, 30000ms)
//                              ^   ^    ^
//                       enabled max  idle_ms
```

Loop terminates when:
- client sends `Connection: close`,
- HTTP/1.0 default,
- `max_requests` reached (0 = unlimited),
- `idle_ms` elapses with no new bytes (0 = use default 30s),
- response status mandates close (408, 426).

Server emits `Connection: keep-alive` and `Keep-Alive: timeout=N,
max=M` headers per response.

---

## Per-connection actor dispatch

```aether
// User actor step function, replaces the thread-pool worker path
@c_callback worker_step(msg_ptr: ptr) {
    msg = unwrap_msg_http_connection(msg_ptr)
    http.server_drain_connection(g_server, msg.client_fd)
}
```

`http.server_drain_connection(server, client_fd)` is the public
helper that runs the full per-connection lifecycle (TLS handshake,
keep-alive request loop, route dispatch, response emission, socket
close). User actor step functions registered via
`http_server_set_actor_handler` should call this on the
`MSG_HTTP_CONNECTION` message's client_fd to get identical behaviour
to the thread-pool worker path.

---

## Middleware

Eleven production middleware in `std.http.middleware`. All registered
via the existing function-pointer chain, hot path stays C function
pointers, no closure indirection.

```aether
import std.http.middleware

// CORS, open it up to one origin
middleware.use_cors(server,
    "https://example.com",
    "GET, POST, OPTIONS",
    "Content-Type, Authorization",
    0,        // allow_credentials
    3600)     // max_age (seconds)

// Per-IP token-bucket rate limit: 100 req per 60s
middleware.use_rate_limit(server, 100, 60000)

// Virtual host gate
middleware.use_vhost(server, "api.example.com,app.example.com")

// Basic auth (verifier is a @c_callback Aether function, receives
// decoded username + password, returns 1 if valid, 0 otherwise)
middleware.use_basic_auth(server, "Restricted", verify_creds_cb, null)

// Bearer token auth, RFC 6750. The verifier receives the raw
// token (the substring after `Bearer `); validation is up to the
// caller (JWT signature check / opaque-token DB lookup / OAuth
// introspection). On failure the response is 401 with
// `WWW-Authenticate: Bearer realm="api"[, error="invalid_token"]`
// so RFC 6750-aware clients can distinguish "no credentials"
// from "bad credentials."
middleware.use_bearer_auth(server, "api", verify_token_cb, null)

// Session-cookie auth, reads a named cookie and hands the value
// to a verifier (DB lookup / signed-token verify). On failure,
// when redirect_url is non-empty, browsers get a 302 to the
// login page; pass "" to return a JSON-API-style 401 instead.
middleware.use_session_auth(server, "SESSIONID", "/login",
                             verify_session_cb, null)

// Response-side gzip (skips bodies < min_size, skips when client
// did not advertise Accept-Encoding: gzip, etc)
middleware.use_gzip(server, 1024, 6)

// Static files
middleware.use_static_files(server, "/assets", "/var/www/static")

// URL rewriting
opts = aether_rewrite_opts_new()
middleware.rewrite_add_rule(opts, "/old/", "/new/")
middleware.use_rewrite(server, opts)

// Custom error pages
ep = aether_error_pages_opts_new()
middleware.error_pages_register(ep, 404, "<h1>Not found</h1>", "text/html")
middleware.error_pages_register(ep, 500, "<h1>Server error</h1>", "text/html")
middleware.use_error_pages(server, ep)

// Real-IP / X-Forwarded-For, extracts the original client IP
// when the server is behind a load balancer / reverse proxy / CDN.
// Reads the configured header (default "X-Forwarded-For"), takes
// the leftmost IP, and adds X-Real-IP to the request so downstream
// handlers / ratelimit / access logs see the real client identity.
// Pass "" for the default header name; alternatives include
// "Forwarded", "CF-Connecting-IP", "True-Client-IP".
middleware.use_real_ip(server, "")
```

Middleware shape: each is a function-pointer chain entry. Pre-handler
middleware (cors / basic_auth / rate_limit / vhost / static_files /
rewrite / real_ip) run before route dispatch and can short-circuit;
response transformers (gzip / error_pages) run after the route
handler emits the response. Order is registration order.

**Real-IP trust model.** `use_real_ip` does NOT validate that the
request actually came through a trusted proxy. Operators must only
run it behind trusted edge infrastructure that strips any
client-supplied X-Forwarded-For, typical setups are a firewall-
restricted port plus a load-balancer rule, or a CDN that overwrites
X-Forwarded-For. Without that guarantee, callers can spoof their
apparent client IP. The middleware is idempotent, running it
twice (or running it after the edge already set X-Real-IP) doesn't
double-tag the request.

---

## Reverse proxy

`std.http.proxy` adds the outbound side: forward inbound requests
to a pool of upstream HTTP servers. nginx-class feature set,
weighted load balancing (round-robin / least-conn / ip-hash /
smooth weighted RR), active health checks, in-memory LRU response
cache with RFC 7234 cacheability rules, per-upstream circuit
breaker, and Hop-by-Hop header handling per RFC 7230.

```aether
import std.http
import std.http.proxy

main() {
    server = http.server_create(8080)
    err = proxy.mount_simple(server, "/", "http://localhost:9000", 30)
    if err != "" { println("proxy: ${err}"); return }
    http.server_start(server)
}
```

For the production pool shape (weighted RR, health checks, cache,
circuit breaker), see [Reverse Proxy](http-reverse-proxy.md).

---

## CONNECT Tunnels

Handlers for `CONNECT` can accept the request and take over the
underlying cleartext HTTP/1.1 connection as a `std.tcp` socket. This
is the primitive for forward-proxy tunnels and other protocols where
HTTP is only the setup handshake.

```aether
import std.http
import std.tcp

handle_connect(req: ptr, res: ptr, ud: ptr) {
    authority = http.request_path(req)  // e.g. "example.com:443"
    if allowed(authority) == 0 {
        http.response_set_status(res, 403)
        http.response_set_body(res, "forbidden")
        return
    }

    http.response_set_status(res, 200)
    tunnel = http.response_accept_tunnel(res)
    if tunnel == null { return }

    // The HTTP server has stopped managing this connection.
    // Relay with tcp.read_n / tcp.write_n, then close it.
    tcp.close(tunnel)
}

http.server_add_route(server, "CONNECT", "*", handle_connect, 0)
```

`response_accept_tunnel` sends the current response head immediately,
then transfers socket ownership to the handler. The normal HTTP
serializer, response transformers, keep-alive handling, and server
socket close are skipped for that connection. Rejected requests still
use the ordinary response path.

The returned handle is a normal `std.tcp` socket, so use
`tcp.read_n` / `tcp.write_n` when the tunnel may carry binary bytes
or embedded NULs. It returns `null` when called outside a live
HTTP/1.1 request, on a TLS-wrapped server connection, or if the HTTP
parser has already buffered bytes that a raw socket could not replay.

---

## WebSocket (RFC 6455)

```aether
@c_callback echo_handler(req: ptr, ws: ptr, ud: ptr) {
    kind = http.ws_recv(ws)         // 1 = text, 2 = binary, -1 = closed
    if kind != -1 {
        msg = http.ws_message(ws)
        http.ws_send_text(ws, "echo: ${msg}")
    }
}

http.server_websocket(server, "/echo", echo_handler, null)
```

Same handler-owns-the-connection shape as SSE. The server runs the
upgrade handshake first (computes
`Sec-WebSocket-Accept = Base64(SHA-1(client_key + magic-uuid))`),
sends `101 Switching Protocols`, then hands an `HttpWsConn*` to the
handler. Inside the handler:

| Call | Behaviour |
|---|---|
| `ws_recv(ws)` | block for next data frame; returns `1` (text), `2` (binary), `-1` (closed). Auto-handles ping/pong; reassembles continuation frames. |
| `ws_message(ws)` | the message contents from the most recent `ws_recv` (NUL-terminated for text). |
| `ws_message_length(ws)` | byte length of the most recent message. |
| `ws_send_text(ws, text)` | emit one text frame; `0` on success, `-1` on transport error. |
| `ws_send_binary(ws, data, len)` | emit one binary frame, binary-safe via explicit length. |
| `ws_close(ws, code, reason)` | emit a close frame; `1000` = normal, `1001` = going away, `1011` = internal error. |

Returning from the handler closes with code `1000` automatically.

## Server-Sent Events

```aether
@c_callback events_handler(req: ptr, sse: ptr, ud: ptr) {
    http.sse_send(sse, "tick", "1")
    http.sse_send(sse, "tick", "2")
    http.sse_send_id(sse, "tick", "3", "evt-3")
    // Falling out of the handler closes the connection.
}

http.server_sse(server, "/events", events_handler, null)
```

The handler owns the connection lifetime. Server emits the standard
SSE response head (`200 OK`, `Content-Type: text/event-stream`,
`Cache-Control: no-cache`, `Connection: close`) before invoking the
handler. `sse_send` returns -1 on transport error so the handler can
stop emitting on disconnect.

Multi-line `data:` payloads are split on `\n` per the spec. Optional
`id:` field via `sse_send_id` lets clients reconnect with
`Last-Event-ID`.

---

## Structured access logging

```aether
http.server_set_access_log(server, "json", "/var/log/api.log")
//                                  ^         ^
//                            "json" or       file path,
//                            "combined"      "-" for stderr,
//                            (NCSA fmt)      "" disables
```

JSON format example output:

```
{"ts":"2026-04-27T11:32:14Z","method":"GET","path":"/api/users",
 "status":200,"bytes":342,"dur_us":1247,"ua":"curl/8.4.0","ref":""}
```

NCSA Combined Log Format is the Apache default. File is opened
`"ab"` so process restarts append rather than truncate.

---

## Prometheus metrics

```aether
http.server_set_metrics(server, "/metrics")
```

Exposes per-route counters and latency histograms on the configured
endpoint:

```
# TYPE aether_http_requests_total counter
# TYPE aether_http_errors_total counter
# TYPE aether_http_4xx_total counter
# TYPE aether_http_request_duration_seconds histogram
aether_http_requests_total{method="GET",path="/api/users"} 4239
aether_http_errors_total{method="GET",path="/api/users"} 12
aether_http_request_duration_seconds_bucket{method="GET",path="/api/users",le="0.005"} 3801
aether_http_request_duration_seconds_bucket{method="GET",path="/api/users",le="0.025"} 4220
...
aether_http_request_duration_seconds_sum{method="GET",path="/api/users"} 47.392011
aether_http_request_duration_seconds_count{method="GET",path="/api/users"} 4239
```

Histogram buckets: 5ms / 25ms / 100ms / 500ms / 2s / 10s / +Inf.
Counters are atomic; insertion-time mutex only.

Custom hooks: `http_server_use_request_hook(server, hook, ud)` is a
C-level extension point (declared in `std/net/aether_http_server.c`,
not exported from `std.http`) used internally by the access-log and
metrics hooks. Multiple hooks may register; they fire in registration
order after the response transformer chain. It is not currently
callable from Aether code.

---

## Graceful shutdown

```aether
err = http.server_shutdown_graceful(server, 5000ms)  // 5-second deadline
if err != "" { println("warning: ${err}") }       // "timeout" if not drained
```

Stops accepting new connections (closes the listen socket), then
waits up to the supplied `Duration` for in-flight ones to drain. Backed by an
atomic `inflight_connections` counter accurate across both
thread-pool and actor-dispatch modes. Typically wired into a
SIGTERM handler.

---

## Embedded / background servers

`http_server_start_background_raw(server)` runs the accept loop on a
detached thread and returns immediately, so a host can keep serving
while doing other work (and stop it later with `http_server_stop`).
This is the *embedded* mode and behaves like a library, not a CLI:

- **No banner.** It does not print `Server running at …` /
  `Press Ctrl+C to stop` an embedded server is controlled by
  `http_server_stop`, not a terminal. (Foreground `server_start` still
  prints the banner.)
- **Quiet signals.** Its detached accept/worker threads block async
  signals (`pthread_sigmask` SIGURG, SIGINT, SIGTERM, SIGPIPE, …), so
  the server's threads never intercept a process-directed signal meant
  for the host application; the synchronous fault signals
  (SEGV/FPE/BUS/…) stay deliverable.

**Reap it, don't leave it lingering.** A backgrounded server left alive
past the end of a command is a finite process the surrounding harness
(a sandboxed agent, CI runner, build tool) must still account for; some
supervisors charge the *foreground* command a non-zero/SIGURG exit for a
lingering multi-threaded child. Always tear an embedded server down
explicitly, `http_server_stop` (or `server_shutdown_graceful`), then
let the process exit, rather than orphaning it.

---

## Lifecycle hooks

```aether
@c_callback on_started(server: ptr, ud: ptr) {
    println("server is ready")
}
@c_callback on_stopped(server: ptr, ud: ptr) {
    println("server stopped")
}

http.server_on_start(server, on_started, null)
http.server_on_stop (server, on_stopped, null)
```

`on_start` fires once after the listen socket is bound, before the
accept loop runs. `on_stop` fires after the accept loop exits, before
sockets close.

---

## Health probes

```aether
@c_callback ready_check(ud: ptr) -> int {
    return database_is_healthy()
}

http.server_set_health_probes(server, "/healthz", "/readyz", ready_check, null)
```

Registers two routes:
- `/healthz` always 200 "ok" (process is up)
- `/readyz` calls `ready_check` (1 → 200 "ready", 0 → 503 "not ready")

Either path may be `""` to skip; `ready_check` may be null. Mirrors
Kubernetes / Docker probe conventions.

---

## Performance tuning checklist

- TLS termination: `AETHER_HAS_OPENSSL` enables it; cert + key must
  be PEM. SNI / ALPN advertising for HTTP/2 lands in the follow-up
  HTTP/2 PR.
- Keep-alive: turn it on for any server fronting a chatty client
  (browsers, internal microservices). `max_requests` of 100–1000 +
  `idle_ms` of 30–60s is the standard tuning.
- Per-connection actor dispatch: only matters for keep-alive
  sessions and SSE/WebSocket. Default thread-pool path is fine for
  short-lived requests.
- gzip: turn on for text responses ≥ 1KB. Skip for already-compressed
  content (images, video), the middleware skips automatically based
  on the absence of the `Content-Encoding` header from the handler.
- Response transformer chain: keep transformers cheap. They run on
  every response; expensive transforms (template rendering, image
  manipulation) belong in route handlers, not transformers.
