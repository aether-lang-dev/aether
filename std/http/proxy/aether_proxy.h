/* std.http.proxy — nginx-class reverse-proxy stack.
 *
 * Closes #381. Adds the outbound half of the HTTP server: forward
 * inbound requests to a pool of upstream HTTP servers, with
 * weighted load balancing, active health checks, in-memory LRU
 * response cache, and a per-upstream circuit breaker.
 *
 * The proxy is a middleware (`int (HttpRequest*, HttpServerResponse*,
 * void* user_data) -> int`) that short-circuits the chain (returns 0)
 * after populating the response. It reuses std.http.client's
 * round-trip path (TLS, header lookup, timeouts, error
 * classification) for the upstream call rather than rolling new
 * sockets.
 *
 * Aether-side surface in std/http/proxy/module.ae. Implementation
 * split per concern across:
 *
 *   aether_proxy_pool.c        AetherUpstream + AetherProxyPool lifecycle
 *   aether_proxy_lb.c          RR / least-conn / ip-hash / weighted-RR
 *   aether_proxy_health.c      health-check thread (one per pool)
 *   aether_proxy_breaker.c     circuit-breaker per-upstream state machine
 *   aether_proxy_cache.c       LRU response cache + TTL eviction
 *   aether_proxy_middleware.c  glue: pick → cache → breaker → forward
 *
 * Threading uses runtime/utils/aether_thread.h (pthread shim that
 * works on Windows-MinGW too) — never <pthread.h> directly.
 */
#ifndef AETHER_PROXY_H
#define AETHER_PROXY_H

#include "../../net/aether_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Upstream pool + load balancing ----- */

typedef struct AetherProxyPool AetherProxyPool;
typedef struct AetherProxyOpts AetherProxyOpts;
typedef struct AetherProxyCache AetherProxyCache;

/* Load-balancer algorithm. Strings are accepted at the Aether
 * boundary; aether_proxy_lb_algo_from_string maps to the enum. */
typedef enum {
    AETHER_PROXY_LB_ROUND_ROBIN  = 0,
    AETHER_PROXY_LB_LEAST_CONN   = 1,
    AETHER_PROXY_LB_IP_HASH      = 2,
    AETHER_PROXY_LB_WEIGHTED_RR  = 3,
    AETHER_PROXY_LB_COOKIE_HASH  = 4,
} AetherProxyLbAlgo;

/* Parse "round_robin" | "least_conn" | "ip_hash" | "weighted_rr".
 * Returns -1 on unknown algorithm. */
int aether_proxy_lb_algo_from_string(const char* name);

/* Allocate an empty pool. Pool refcount starts at 1 (the caller).
 * `request_timeout_sec` and `dial_timeout_ms` apply to every
 * upstream. `max_inflight_per_up` caps concurrent in-flight
 * requests per upstream (over-cap upstreams are skipped by the
 * picker). 0 disables the cap.
 *
 * Returns NULL on bad inputs (negative timeouts, unknown algo) or
 * OOM. */
AetherProxyPool* aether_proxy_pool_new(AetherProxyLbAlgo algo,
                                       int request_timeout_sec,
                                       int dial_timeout_ms,
                                       int max_inflight_per_up);

/* Decrement refcount; free on zero. Joins the health-check thread
 * if one was started. NULL-safe. */
void aether_proxy_pool_free(AetherProxyPool* pool);

/* Add an upstream by base URL. weight is meaningful only under
 * weighted_rr (default 1 elsewhere). Duplicate URLs produce a
 * non-empty error string. */
const char* aether_proxy_upstream_add(AetherProxyPool* pool,
                                      const char* base_url,
                                      int weight);

/* Remove a previously-added upstream. Idempotent (returns "" if
 * absent). */
const char* aether_proxy_upstream_remove(AetherProxyPool* pool,
                                         const char* base_url);

/* 1 when every upstream of the pool `opts` is mounted with is plain HTTP:
 * what the proxy event loop dials itself (#3231). A mount with an https
 * upstream is served on the per-connection path, whose client speaks TLS. */
int aether_proxy_upstreams_all_plain(const AetherProxyOpts* opts);

/* Active drain mode. After `drain` is called, the LB picker
 * skips the upstream for new requests; in-flight requests
 * complete naturally. `undrain` reverses the flag.
 *
 * Returns "" on success, error string when the upstream isn't in
 * the pool. Idempotent — drain on an already-draining upstream
 * is a no-op. Useful for rolling deploys: drain → wait for
 * inflight → 0 → physically remove. */
const char* aether_proxy_upstream_drain(AetherProxyPool* pool,
                                        const char* base_url);
const char* aether_proxy_upstream_undrain(AetherProxyPool* pool,
                                          const char* base_url);

/* Per-upstream rate limit (token bucket). max_rps is steady-state
 * requests per second per upstream; burst is the maximum tokens
 * the bucket can hold. Setting max_rps=0 disables (default).
 * Distinct from the per-client middleware rate limit — this one
 * protects upstreams from overload regardless of client identity.
 *
 * Applies to ALL upstreams currently in the pool plus any added
 * later. */
const char* aether_proxy_rate_limit_set(AetherProxyPool* pool,
                                        int max_rps,
                                        int burst);

/* Cookie-name configuration for the COOKIE_HASH algorithm. The
 * pool reads this cookie's value from the inbound request and
 * hashes it (FNV-1a) to pick an upstream. Must be set before the
 * first request hits the LB picker; otherwise COOKIE_HASH falls
 * back to RR. */
/* The client pool holding this proxy pool's idle upstream connections,
 * borrowed: it lives as long as the proxy pool. Read and clear it with the
 * http_client_pool_* calls (aether_http.h); never free it. */
struct HttpClientPool* aether_proxy_pool_conns(AetherProxyPool* pool);

const char* aether_proxy_pool_set_cookie_name(AetherProxyPool* pool,
                                              const char* cookie_name);

/* ----- Health checks (background pthread per pool) ----- */

/* Configure + start the health-check thread for the pool. The
 * thread fires `GET probe_path` against each upstream every
 * `interval_ms` (with `timeout_ms` per probe), tallies wins/
 * losses, and flips the upstream's healthy state once the
 * threshold is crossed. healthy=0 makes the LB picker skip the
 * upstream until it recovers.
 *
 *   expect_status     — 0 = any 2xx; otherwise the exact status.
 *   interval_ms       — between probes.
 *   timeout_ms        — per-probe HTTP timeout.
 *   healthy_threshold — N consecutive OK probes to flip up.
 *   unhealthy_threshold — N consecutive failed probes to flip down.
 *
 * Returns "" on success, error string on bad inputs / thread
 * spawn failure. Calling twice on the same pool stops the prior
 * thread and starts a new one with the new config. */
const char* aether_proxy_health_checks_enable(AetherProxyPool* pool,
                                              const char* probe_path,
                                              int expect_status,
                                              int interval_ms,
                                              int timeout_ms,
                                              int healthy_threshold,
                                              int unhealthy_threshold);

/* ----- Circuit breaker (per-upstream state, per-pool config) ----- */

const char* aether_proxy_breaker_configure(AetherProxyPool* pool,
                                           int failure_threshold,
                                           int open_duration_ms,
                                           int half_open_max);

/* ----- Response cache (in-memory LRU + TTL) ----- */

typedef enum {
    AETHER_PROXY_CACHE_KEY_URL              = 0,  /* method-agnostic */
    AETHER_PROXY_CACHE_KEY_METHOD_URL       = 1,
    AETHER_PROXY_CACHE_KEY_METHOD_URL_VARY  = 2,
} AetherProxyCacheKeyStrategy;

int aether_proxy_cache_key_strategy_from_string(const char* name);

AetherProxyCache* aether_proxy_cache_new(int max_entries,
                                         int max_body_bytes,
                                         int default_ttl_sec,
                                         AetherProxyCacheKeyStrategy key_strategy);
void aether_proxy_cache_free(AetherProxyCache* cache);

/* ----- Per-mount options ----- */

AetherProxyOpts* aether_proxy_opts_new(void);
void aether_proxy_opts_free(AetherProxyOpts* opts);

/* When non-empty, chop this prefix from `req->path` before
 * forwarding. Empty string disables the chop. */
const char* aether_proxy_opts_set_strip_prefix(AetherProxyOpts* opts,
                                               const char* prefix);

/* preserve_host=0 → rewrite Host: to upstream's host (default).
 * preserve_host=1 → forward client Host: verbatim. */
const char* aether_proxy_opts_set_preserve_host(AetherProxyOpts* opts, int on);

/* X-Forwarded-* injection toggles. Each non-zero arg enables that
 * header; defaults are all on (xff=1, xfp=1, xfh=1). */
const char* aether_proxy_opts_set_xforwarded(AetherProxyOpts* opts,
                                             int xff, int xfp, int xfh);

const char* aether_proxy_opts_bind_cache(AetherProxyOpts* opts,
                                         AetherProxyCache* cache);

/* Default 8 MiB. Larger requests/responses hit a 502 Bad Gateway
 * (proxy buffers the body; oversize is rejected before allocation
 * to avoid OOM under load). */
const char* aether_proxy_opts_set_body_cap(AetherProxyOpts* opts,
                                           int max_body_bytes);

/* Idempotent retry policy. When max_retries > 0, the middleware
 * retries failed upstream calls (5xx + transport errors) for
 * idempotent methods (GET, HEAD, PUT, DELETE, OPTIONS) using
 * exponential backoff with full jitter, starting at backoff_base_ms.
 * Non-idempotent methods (POST, PATCH) are NEVER retried —
 * at-most-once delivery is preserved by HTTP semantics.
 *
 *   max_retries:      additional attempts after the first failure
 *                     (so total upstream calls = 1 + max_retries).
 *                     0 (default) disables retry.
 *   backoff_base_ms:  starting delay; doubles each retry up to a
 *                     cap; full-jitter random in [0, current_backoff].
 *                     Default 100 ms when max_retries > 0. */
const char* aether_proxy_opts_set_retry_policy(AetherProxyOpts* opts,
                                               int max_retries,
                                               int backoff_base_ms);

/* W3C Trace-Context propagation. When `inject = 1`, the proxy
 * generates a fresh `traceparent` (00 + new trace-id + new
 * span-id + sampled=01) when the inbound request carries none —
 * unblocking distributed tracing for clients that aren't
 * trace-aware. When inject = 0 (default), the proxy passes
 * inbound `traceparent` and `tracestate` through verbatim
 * without generating new ones (preserves existing client-side
 * trace continuity). */
const char* aether_proxy_opts_set_trace_inject(AetherProxyOpts* opts,
                                               int inject);

/* ----- Install on server ----- */

/* Mount the proxy under `path_prefix` ("/" forwards everything;
 * "/api" forwards just the /api subtree). The opts' refcount on
 * the pool is bumped (and decremented on opts_free). The middleware
 * is registered with the existing http_server_use_middleware
 * chain — order with other middleware (real_ip, gzip, etc.) is
 * registration order. */
const char* aether_proxy_use_reverse_proxy(HttpServer* server,
                                           const char* path_prefix,
                                           AetherProxyPool* pool,
                                           AetherProxyOpts* opts);

/* Method-filtered mount. `methods_csv` is a comma/space-separated
 * allow-list such as "GET,HEAD" or "POST, PUT, DELETE". Requests
 * whose method is not listed pass through to later middleware/routes.
 * Middleware registration order is preserved, so first matching
 * proxy mount wins. */
const char* aether_proxy_use_reverse_proxy_methods(HttpServer* server,
                                                   const char* path_prefix,
                                                   AetherProxyPool* pool,
                                                   AetherProxyOpts* opts,
                                                   const char* methods_csv);

/* Method + route-pattern mount. `path_pattern` uses the same shape
 * as std.http routes (`/repos/:repo/info`, `/assets/<wildcard>`). Unlike
 * `path_prefix`, it matches the whole request path before forwarding.
 * Useful for carve-outs before broader method mounts. */
const char* aether_proxy_use_reverse_proxy_match(HttpServer* server,
                                                 const char* path_pattern,
                                                 AetherProxyPool* pool,
                                                 AetherProxyOpts* opts,
                                                 const char* methods_csv);

/* One-upstream convenience. Builds a pool with one upstream + RR +
 * default opts, mounts the middleware, and returns "". The pool
 * is owned by the middleware and freed alongside the server. */
const char* aether_proxy_use_simple_proxy(HttpServer* server,
                                          const char* path_prefix,
                                          const char* upstream_url,
                                          int request_timeout_sec);

/* Internal — exposed only because the middleware needs it. The
 * function pointer to register with http_server_use_middleware. */
int aether_middleware_reverse_proxy(HttpRequest* req,
                                    HttpServerResponse* res,
                                    void* user_data);

/* Render the proxy's metric snapshot to a Prometheus-compatible
 * text body. Output mirrors the existing
 * http_server_set_metrics scheme:
 *
 *   aether_proxy_upstream_requests_total{upstream="...",class="2xx"} N
 *   aether_proxy_upstream_transport_errors_total{upstream="..."} N
 *   aether_proxy_upstream_timeouts_total{upstream="..."} N
 *   aether_proxy_upstream_retries_total{upstream="..."} N
 *   aether_proxy_upstream_latency_ms_sum{upstream="..."} N
 *   aether_proxy_upstream_latency_ms_count{upstream="..."} N
 *   aether_proxy_upstream_inflight{upstream="..."} N
 *   aether_proxy_upstream_healthy{upstream="..."} 0|1
 *   aether_proxy_upstream_breaker_state{upstream="..."} 0|1|2
 *   aether_proxy_upstream_draining{upstream="..."} 0|1
 *   aether_proxy_cache_hits_total N
 *   aether_proxy_cache_misses_total N
 *   aether_proxy_cache_revalidations_total N
 *   aether_proxy_503_no_upstream_total N
 *
 * A fresh counted AetherString (cast to char*) the caller releases;
 * NULL on OOM. */
char* aether_proxy_pool_metrics_text(AetherProxyPool* pool);

#ifdef __cplusplus
}
#endif

#endif  /* AETHER_PROXY_H */
