# `std.http.proxy` reverse proxy

nginx-class outbound HTTP forwarding for the Aether server.
Forward inbound requests to a pool of upstream HTTP servers, with:

- five load-balancing algorithms (round-robin, least-conn, ip-hash,
  smooth-weighted RR, cookie-hash sticky)
- active health checks (one thread per pool, lock-free hot-path read)
- per-upstream circuit breaker (closed / open / half-open)
- in-memory LRU response cache with Vary-aware keying and TTL eviction
- idempotent retry on 5xx + transport with exponential backoff +
  full jitter, re-picks a different upstream per attempt (nginx
  `proxy_next_upstream` semantics)
- per-upstream token-bucket rate limiting
- active drain (take a host out of rotation without removing it
  from the pool)
- W3C Trace-Context propagation (passthrough or generated traceparent)
- Hop-by-Hop header handling per RFC 7230 §6.1
- Prometheus 0.0.4 metrics surface (per-upstream + per-pool)

The proxy is a middleware plugged into the existing
`http_server_use_middleware` chain. It short-circuits the chain
(returns 0) and owns the response.

## Quick start, single upstream

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

`mount_simple(server, path_prefix, upstream_url, timeout_sec)`
builds a single-upstream pool with round-robin (no other upstream
to balance against), default opts, and the given timeout. Mounts
the middleware on `path_prefix`. `"/"` forwards everything;
`"/api"` forwards just the `/api` subtree.

## Pool form, load balancing + health + cache + breaker

```aether
import std.http
import std.http.proxy

main() {
    server = http.server_create(8080)

    // Three upstreams, smooth weighted RR with 3:2:1 ratio.
    pool = proxy.upstream_pool_new("weighted_rr", 30, 0, 100)
    proxy.upstream_add(pool, "http://10.0.0.1:8080", 3)
    proxy.upstream_add(pool, "http://10.0.0.2:8080", 2)
    proxy.upstream_add(pool, "http://10.0.0.3:8080", 1)

    // Active health checks every 5s. Two consecutive 200s flips an
    // upstream up; three consecutive failures flips it down.
    proxy.health_checks_enable(pool, "/health", 200, 5000, 1000, 2, 3)

    // Per-upstream circuit breaker: 5 consecutive failures opens the
    // breaker for 30s; one half-open test request closes it on success.
    proxy.breaker_configure(pool, 5, 30000, 1)

    // Response cache: 1000 entries, 64 KiB body cap, 60s default TTL,
    // canonical key includes Vary headers from upstream.
    cache = proxy.cache_new(1000, 65536, 60, "method_url_vary")

    // Per-mount options.
    opts = proxy.opts_new()
    proxy.opts_bind_cache(opts, cache)
    proxy.opts_set_strip_prefix(opts, "/api")  // /api/users → /users upstream

    err = proxy.mount(server, "/api", pool, opts)
    if err != "" { println("proxy: ${err}"); return }

    http.server_start(server)
}
```

## Method-Aware Routing

Multiple proxy mounts can share one listener. Registration order is
execution order; a mount whose method or path pattern does not match
passes through to the next middleware, so narrower carve-outs should
be registered first.

```aether,fragment
proxy.mount_match(server, "/repos/:repo/info", primary_pool, primary_opts, "GET,HEAD")
proxy.mount_methods(server, "/repos", primary_pool, write_opts, "POST,PUT,DELETE")
proxy.mount_methods(server, "/repos", replica_pool, read_opts, "GET,HEAD")
```

`mount_methods` keeps the normal path-prefix match. `mount_match`
uses std.http route-pattern syntax (`:param`, `*`) against the whole
request path, which covers cases like `GET /repos/:repo/info` going
to a primary while other reads fan out to replicas.

## Load-balancing algorithms

| Algorithm | Selection rule | When to pick |
|---|---|---|
| `round_robin` | atomic counter mod N (skips ineligible upstreams via retry) | default, even distribution, no per-request decision cost |
| `least_conn` | iterate eligible upstreams; pick smallest in-flight count | upstream response times vary; long-running requests should avoid stacking on one upstream |
| `ip_hash` | FNV-1a over X-Forwarded-For (or X-Real-IP, or "anonymous") mod N | sticky sessions for clients without explicit cookies; soft sticky, falls back to RR if the natural pick is ineligible |
| `weighted_rr` | smooth weighted RR (the algorithm nginx uses) | mixed-capacity backends; produces interleaved sequences (3:1 → A,A,A,B,A,A,A,B) instead of batched |
| `cookie_hash` | FNV-1a over the value of a configured cookie name (set via `proxy.pool_set_cookie_name`) mod N; falls back to RR if the cookie is absent | application-layer sticky sessions where the client carries a session cookie and the upstream pool has per-instance state |

"Eligible" means: `healthy && !draining && breaker not OPEN && inflight < max_inflight_per_up && rate_limit admits`.

```aether,fragment
// cookie-hash sticky: bind requests carrying SESSIONID=foo to the
// same upstream every time. Different cookie values may hash to
// different upstreams; identical values are deterministic.
pool = proxy.upstream_pool_new("cookie_hash", 30, 0, 0)
proxy.upstream_add(pool, "http://10.0.0.1:8080", 1)
proxy.upstream_add(pool, "http://10.0.0.2:8080", 1)
proxy.pool_set_cookie_name(pool, "SESSIONID")
```

## Health checks

One pthread per pool (not per upstream, N-thread blow-up). The
thread iterates the upstream list every `interval_ms`, fires
`GET probe_path` against each `base_url`, classifies the response,
and flips `_Atomic int healthy` once the threshold trips:

- `healthy_threshold` consecutive OK probes flip an upstream up.
- `unhealthy_threshold` consecutive failed probes flip it down.

`expect_status = 0` accepts any 2xx; non-zero is the exact status.
Down upstreams are skipped by the LB picker (every algorithm).

Configure once via `proxy.health_checks_enable(pool, ...)`; calling
again on the same pool stops the prior thread and starts a new one
with the new config. `proxy.upstream_pool_free` joins the thread.

## Circuit breaker

Per-upstream three-state machine:

```
   CLOSED ── consecutive_failures ≥ failure_threshold ──→ OPEN
                                                          │
   OPEN   ── now - opened_at ≥ open_duration_ms ─────────→ HALF_OPEN
                                                          │
   HALF_OPEN ── any test request succeeds ───────────────→ CLOSED
   HALF_OPEN ── any test request fails ──────────────────→ OPEN (reset opened_at)
```

The hot path is one atomic load per request (`cb_state`).
Transitions are CAS-based for the OPEN→HALF_OPEN race so two
threads simultaneously timing out the open window don't both admit
"the test request"; `half_open_max` caps concurrent test requests.

5xx responses and transport errors (DNS, connect, TLS, timeout)
count as failures. 4xx is treated as ok (client error, not upstream
fault).

`failure_threshold = 0` disables the breaker (default, opt in via
`proxy.breaker_configure`).

## Response cache

In-memory LRU with TTL eviction. Open-chained hash + doubly-linked
LRU list. Cacheability gates per RFC 7234 conservative subset:

- Methods: GET, HEAD only.
- Status codes: 200, 203, 204, 300, 301, 404, 410.
- Request `Cache-Control: no-store` → bypass.
- Response `Cache-Control: no-store` / `private` → bypass.
- Response `Vary: *` → uncacheable.
- Body length ≤ `max_body_bytes`.

TTL resolution: response `Cache-Control: max-age` (clamped to 1
hour for v1 conservatism), else `default_ttl_sec`. `s-maxage` and
`Expires` are not consulted in v1.

### Key strategies

| Strategy | Key | Use when |
|---|---|---|
| `url` | `path?query` | method-agnostic, POST/GET share entries (rare; usually wrong) |
| `method_url` | `METHOD path?query` | safe default, GET and HEAD don't share entries with each other or with POST |
| `method_url_vary` | as above + `\0name=value` for each header in upstream's `Vary:` | the response varies on `Accept-Encoding` / `Accept-Language` / etc. |

## Idempotent retry, `proxy_next_upstream` semantics

Off by default. Opt in with:

```aether,fragment
proxy.opts_set_retry_policy(opts, 3, 100)   // max_retries=3, backoff_base_ms=100
```

When a 5xx or transport error comes back from an upstream and the
request is **idempotent** (`GET / HEAD / PUT / DELETE / OPTIONS`),
the middleware:

1. Charges the failure against the failing upstream's circuit
   breaker (so a flapping host trips the breaker normally).
2. Releases the inflight count on that upstream.
3. Calls the load-balancer picker again, usually selecting a
   different upstream.
4. Backs off for `backoff_base_ms × 2^(attempt-1)` with full jitter
   (uniform random in `[0, current_backoff]`), capped at 10 s.
5. Retries.

`POST` and `PATCH` are **never retried**, at-most-once delivery is
preserved so the proxy never silently double-applies a non-idempotent
request. The total retry budget is `1 + max_retries` attempts; if
no eligible upstream remains, the proxy returns 503 with
`X-Aether-Proxy-Error: no_upstream_after_retry` and `Retry-After: 1`.

## Per-upstream rate limit (token bucket)

Off by default. Configure once on the pool:

```aether,fragment
proxy.rate_limit_set(pool, 200, 50)   // max_rps=200, burst=50
```

Each upstream gets an independent token bucket: tokens refill at
`max_rps` per second, capped at `burst`. The LB picker decrements
one token before admitting a request; when the bucket is empty
the upstream is treated as ineligible (RR / weighted-RR / etc.
fall through to the next eligible upstream, or 503 if none).

Set `max_rps = 0` to disable. The burst capacity matters at start-
up: with `burst = 0` the first request is rejected, so for typical
deployments `burst` should be at least equal to expected concurrent
connection bursts (a small multiple of `max_rps` is conservative).

## Active drain

Take a host out of rotation without removing it from the pool:

```aether,fragment
proxy.upstream_drain(pool,   "http://10.0.0.1:8080")   // stop sending new requests
proxy.upstream_undrain(pool, "http://10.0.0.1:8080")   // re-admit
```

Drained upstreams are skipped by the LB picker (every algorithm).
In-flight requests finish naturally; when they complete the
upstream is fully idle and safe to take down for a deploy. Drain
state is operator-driven and orthogonal to `healthy` (which the
health-check thread owns).

## W3C Trace-Context propagation

Two modes per mount:

```aether,fragment
proxy.opts_set_trace_inject(opts, 0)   // default, passthrough
proxy.opts_set_trace_inject(opts, 1)   // generate when missing
```

**Passthrough** (`inject=0`): if the inbound request carries
`traceparent` / `tracestate`, the proxy forwards them unchanged
to the upstream. End-to-end traces propagate naturally. If the
inbound has no `traceparent`, none is added.

**Inject** (`inject=1`): same passthrough behaviour for incoming
trace headers, *plus*, when the inbound request has no
`traceparent` the proxy generates a fresh W3C-compliant one
(`00-<32-hex>-<16-hex>-01`) and stamps it on the outbound request.
Use this on the edge of your service mesh so every upstream gets a
trace context even from trace-naïve clients.

## Prometheus metrics

```aether,fragment
proxy.pool_metrics_text(pool) -> string    // 0.0.4 exposition
```

Wire it to a route to expose Grafana-scrapable metrics:

```aether,fragment
handle_metrics(req: ptr, res: ptr, ud: ptr) {
    body = proxy.pool_metrics_text(ud)
    http.response_set_status(res, 200)
    http.response_set_header(res, "Content-Type", "text/plain; version=0.0.4")
    http.response_set_body(res, body)
}

http.server_get(server, "/proxy-metrics", handle_metrics, pool)
proxy.mount(server, "/api", pool, opts)  // mount AFTER routes
```

(Mount the proxy on a more specific prefix than `"/"` so it
doesn't shadow the metrics route.)

Surface (HELP + TYPE blocks elided in this table):

| Metric | Type | Labels | What |
|---|---|---|---|
| `aether_proxy_upstream_requests_total` | counter | `upstream`, `class` (`2xx`/`3xx`/`4xx`/`5xx`) | Proxied requests by status class |
| `aether_proxy_upstream_transport_errors_total` | counter | `upstream` | DNS / connect / TLS / non-timeout transport failures |
| `aether_proxy_upstream_timeouts_total` | counter | `upstream` | Calls that exceeded `request_timeout_sec` |
| `aether_proxy_upstream_retries_total` | counter | `upstream` | Idempotent retries fired against this upstream |
| `aether_proxy_upstream_latency_ms_sum` | counter | `upstream` | Sum of upstream call durations (ms) |
| `aether_proxy_upstream_latency_ms_count` | counter | `upstream` | Count of upstream calls observed |
| `aether_proxy_upstream_inflight` | gauge | `upstream` | Current concurrent in-flight requests |
| `aether_proxy_upstream_healthy` | gauge | `upstream` | 1 if the health-check thread considers the upstream up |
| `aether_proxy_upstream_breaker_state` | gauge | `upstream` | 0=closed, 1=open, 2=half_open |
| `aether_proxy_upstream_draining` | gauge | `upstream` | 1 if operator-drained |
| `aether_proxy_cache_hits_total` | counter | (none, pool-level) | Cache hits |
| `aether_proxy_cache_misses_total` | counter | (none) | Cache misses |
| `aether_proxy_cache_revalidations_total` | counter | (none) | Conditional GETs that returned 304. Reserved for v2 conditional revalidation; always 0 in v1. |
| `aether_proxy_503_no_upstream_total` | counter | (none) | 503s due to no eligible upstream |

Latency is exposed as `_sum + _count` so a Grafana panel can
compute average latency as `rate(_sum) / rate(_count)`.

## Hop-by-Hop headers (RFC 7230 §6.1)

Stripped on **both** directions:

```
Connection, Keep-Alive, Proxy-Authenticate, Proxy-Authorization,
TE, Trailer, Transfer-Encoding, Upgrade, Proxy-Connection
```

Plus any header listed in the request's `Connection:` value.

## Headers added by the proxy

| Header | Default | Toggle |
|---|---|---|
| `X-Forwarded-For` | client IP appended (preserves prior comma-separated values) | `proxy.opts_set_xforwarded(opts, xff=0, ...)` |
| `X-Forwarded-Proto` | `http` | `xfp=0` |
| `X-Forwarded-Host` | client's `Host:` header | `xfh=0` |
| `Via` | `1.1 aether-proxy` (appended to existing) | always on |
| `Host` | rewritten to upstream's `host:port` | `proxy.opts_set_preserve_host(opts, 1)` to forward client Host: instead |

Client IP is resolved via the same chain `middleware.use_real_ip`
uses: `X-Forwarded-For` (leftmost) → `X-Real-IP` → `"unknown"`.
Mount `middleware.use_real_ip` before the proxy if you want
clients behind a trusted edge to be visible via XFF.

## Error responses

The middleware emits one of these and short-circuits the chain.
Each carries an `X-Aether-Proxy-Error` header for log aggregators:

| Status | Trigger | `X-Aether-Proxy-Error` |
|--:|---|---|
| 502 | upstream connect / DNS / TLS handshake failure | `upstream_transport` |
| 502 | request body exceeds `opts.max_body_bytes` | `request_body_too_large` |
| 502 | upstream response exceeds `opts.max_body_bytes` | `response_too_large` |
| 502 | request carries `Upgrade:` (WebSocket / h2 upstream, v2 follow-up) | `upgrade_unsupported` |
| 503 | LB picker found no eligible upstream (all unhealthy or breaker open). Includes `Retry-After: 1`. | `no_upstream` |
| 504 | `request_timeout_sec` elapsed before upstream responded | `upstream_timeout` |
| 5xx upstream | passed through verbatim, operators see the actual error | (none, body is upstream's) |

## Limitations (v1)

- Request and response bodies are buffered in memory up to
  `opts.max_body_bytes` (default 8 MiB). Streaming pass-through is
  the next major feature.
- WebSocket and SSE upstreams are unsupported: requests with
  `Upgrade:` headers are refused with 502. The upstream can still
  serve Server-Sent Events to clients via the existing `std.http`
  surface, so long as the proxy is not in front of those routes.
- Upstreams are called over HTTP/1.1 via `std.http.client`. Inbound
  HTTP/2 works (the inbound side is independent); upstream HTTP/2 is
  a v2 follow-up.
- **In-memory cache only.** No disk-backed cache, no shared
  cache across processes.
- **Sharded cache lock is a v2 follow-up.** Single mutex around
  the whole cache works under typical workloads; revisit if
  contention shows in profiling.

## Performance

| Cost | Threshold |
|---|---|
| Single-upstream RR throughput | within 50% of direct `std.http.client` at 100 concurrent connections |
| Cache hit lookup | ≤1 µs warm |
| Health-check thread | <0.1% CPU at 1 Hz × 1 upstream |
| Circuit-breaker hot-path read | single relaxed atomic load |
| WRR pick | O(N) under pool mutex; <100 ns at N=10 |

## Reference

```aether,fragment
import std.http.proxy

// ---- Pool + upstream lifecycle
proxy.upstream_pool_new(lb_algo, request_timeout_sec, dial_timeout_ms, max_inflight_per_up) -> ptr
proxy.upstream_pool_free(pool)
proxy.upstream_add(pool, base_url, weight) -> string
proxy.upstream_remove(pool, base_url) -> string
proxy.upstream_drain(pool, base_url) -> string       // skip in LB
proxy.upstream_undrain(pool, base_url) -> string     // re-admit

// ---- Per-pool tuning
proxy.pool_set_cookie_name(pool, name) -> string     // for "cookie_hash"
proxy.rate_limit_set(pool, max_rps, burst) -> string // 0 = disable

// ---- Health checks
proxy.health_checks_enable(pool, probe_path, expect_status,
    interval_ms, timeout_ms, healthy_threshold, unhealthy_threshold) -> string

// ---- Circuit breaker
proxy.breaker_configure(pool, failure_threshold, open_duration_ms, half_open_max) -> string

// ---- Cache
proxy.cache_new(max_entries, max_body_bytes, default_ttl_sec, key_strategy) -> ptr
proxy.cache_free(cache)

// ---- Per-mount opts
proxy.opts_new() -> ptr
proxy.opts_free(opts)
proxy.opts_set_strip_prefix(opts, prefix) -> string
proxy.opts_set_preserve_host(opts, on) -> string
proxy.opts_set_xforwarded(opts, xff, xfp, xfh) -> string
proxy.opts_bind_cache(opts, cache) -> string
proxy.opts_set_body_cap(opts, max_body_bytes) -> string
proxy.opts_set_retry_policy(opts, max_retries, backoff_base_ms) -> string
proxy.opts_set_trace_inject(opts, on) -> string

// ---- Install
proxy.mount(server, path_prefix, pool, opts) -> string
proxy.mount_simple(server, path_prefix, upstream_url, request_timeout_sec) -> string

// ---- Observability
proxy.pool_metrics_text(pool) -> string              // Prometheus 0.0.4
```

All `string`-returning functions are Go-style: `""` is success;
non-empty is an error message.

## See also

- [`docs/http-server.md`](http-server.md), the inbound side
  (routes, middleware, TLS, h2, WS, SSE, health probes,
  metrics, graceful shutdown).
- [`examples/stdlib/http-reverse-proxy.ae`](../examples/stdlib/http-reverse-proxy.ae)
  minimal single-upstream demo.
- [`examples/stdlib/http-reverse-proxy-pool.ae`](../examples/stdlib/http-reverse-proxy-pool.ae)
  full enterprise stack: pool, health, breaker, cache, retry,
  rate limit, drain, trace-context, Prometheus metrics.
