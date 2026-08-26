/* aether_proxy_internal.h — shared definitions for the proxy
 * implementation files. Not part of the public surface; included
 * only by std/http/proxy/aether_proxy_*.c. Layout decisions live
 * here so each concern's .c file can mutate the right fields
 * with the right locking discipline.
 *
 * Locking summary:
 *
 *   AetherProxyPool::lock      — guards upstream array + WRR state.
 *                                Held briefly during pick; not held
 *                                across the upstream HTTP call.
 *   AetherUpstream fields      — atomics for healthy / inflight /
 *                                cb_state / cb_consecutive_failures /
 *                                cb_opened_at_ms / cb_half_open_inflight
 *                                so the LB picker is lock-free in the
 *                                hot path.
 *   AetherProxyPool::hc_*      — hc_cv_lock + hc_cv only used to wake
 *                                the health-check thread on shutdown.
 *   AetherProxyCache::lock     — single mutex around the hash + LRU
 *                                list. Sharded locking is a v2
 *                                optimisation when contention shows.
 */
#ifndef AETHER_PROXY_INTERNAL_H
#define AETHER_PROXY_INTERNAL_H

#include "aether_proxy.h"
#include "../../net/aether_http.h"

#include "../../../runtime/utils/aether_thread.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Three-state machine for the per-upstream circuit breaker.
 * Storage: _Atomic int. Transitions are explicit and CAS-based
 * for the open→half_open path so two threads racing don't both
 * admit "the test request". */
typedef enum {
    AETHER_PROXY_CB_CLOSED    = 0,
    AETHER_PROXY_CB_OPEN      = 1,
    AETHER_PROXY_CB_HALF_OPEN = 2,
} AetherProxyBreakerState;

/* host:port of a base URL, malloc'd. Defined in aether_proxy_middleware.c;
 * the pool calls it once per upstream at registration. */
char* aether_proxy_authority_of(const char* url);

typedef struct AetherUpstream {
    char* base_url;                   /* "http://10.0.0.1:8080" — owned */
    char* authority;                  /* "10.0.0.1:8080" — the host:port slice
                                       * of base_url, owned. Derived once here
                                       * because base_url cannot change while
                                       * the upstream exists, and the request
                                       * path needs it on every forward. */
    int   weight;                     /* WRR; default 1 */

    /* Smooth weighted-RR running state — mutated under pool->lock. */
    int   current_weight;
    int   effective_weight;

    /* Health state. Atomic so the LB picker reads without locking. */
    _Atomic int healthy;              /* 1 = up, 0 = down. Init 1. */
    _Atomic int consecutive_ok;       /* updated by health-check thread */
    _Atomic int consecutive_fail;

    /* Active drain. When set, the LB picker treats this upstream as
     * ineligible for new requests; in-flight requests finish
     * naturally. Flipped by aether_proxy_upstream_drain /
     * _undrain. Distinct from `healthy` (which the health-check
     * thread owns) — drain is operator-driven. */
    _Atomic int draining;

    /* Inflight counter — least-conn picker reads, middleware bumps
     * around the upstream call (incref before, decref after). */
    _Atomic int inflight;

    /* Circuit-breaker per-upstream state. cb_state is the hot-path
     * read; updates only on response classification. */
    _Atomic int    cb_state;          /* AetherProxyBreakerState */
    _Atomic int    cb_consecutive_failures;
    _Atomic long   cb_opened_at_ms;   /* monotonic clock ms */
    _Atomic int    cb_half_open_inflight;

    /* Per-upstream token-bucket rate limit. Disabled when
     * `rl_max_rps == 0` (default). Otherwise admits up to
     * `rl_max_rps` requests/second per upstream with `rl_burst`
     * burst capacity. The picker calls aether_proxy_rate_limit_admit
     * which atomically refills the bucket and decrements it.
     * Refill state is guarded by rl_lock (held only briefly, no
     * upstream call inside). */
    pthread_mutex_t rl_lock;
    int             rl_max_rps;       /* 0 = disabled */
    int             rl_burst;
    double          rl_tokens;        /* current token count */
    long            rl_last_refill_ms;

    /* Per-upstream metrics counters. _Atomic so the metrics
     * scraper reads them without taking any pool lock. */
    _Atomic long    metric_requests_2xx;
    _Atomic long    metric_requests_3xx;
    _Atomic long    metric_requests_4xx;
    _Atomic long    metric_requests_5xx;
    _Atomic long    metric_transport_errors;
    _Atomic long    metric_timeouts;
    _Atomic long    metric_retries;
    _Atomic long    metric_latency_sum_ms;
    _Atomic long    metric_latency_count;
} AetherUpstream;

struct AetherProxyPool {
    AetherProxyLbAlgo algo;
    int   request_timeout_sec;
    int   dial_timeout_ms;
    int   max_inflight_per_up;        /* 0 = uncapped */

    /* For algo=COOKIE_HASH only: which cookie name carries the
     * stickiness key. NULL when algo != COOKIE_HASH. */
    char* cookie_name;

    /* Pool-level cache metric counters (cache itself doesn't know
     * which pool it serves; the middleware records into these). */
    _Atomic long  metric_cache_hits;
    _Atomic long  metric_cache_misses;
    _Atomic long  metric_cache_revalidations;
    _Atomic long  metric_503_no_upstream;

    /* Upstream array. Fixed-grow; modest N (handful of upstreams) so
     * realloc-on-add is fine. */
    pthread_mutex_t lock;
    AetherUpstream** upstreams;
    int    upstream_count;
    int    upstream_cap;
    _Atomic int rr_cursor;            /* RR doesn't need the lock */

    /* Health-check thread (one per pool — see aether_proxy_health.c). */
    int             hc_started;
    pthread_t       hc_thread;
    _Atomic int     hc_stopping;
    pthread_mutex_t hc_cv_lock;
    pthread_cond_t  hc_cv;
    char*           hc_probe_path;
    int             hc_expect_status;
    int             hc_interval_ms;
    int             hc_timeout_ms;
    int             hc_healthy_threshold;
    int             hc_unhealthy_threshold;

    /* Circuit-breaker config. Per-upstream state lives on AetherUpstream;
     * config (thresholds + duration) is per-pool because the policy is
     * a deployment-level decision, not a per-upstream one. */
    int br_failure_threshold;         /* 0 = breaker disabled */
    int br_open_duration_ms;
    int br_half_open_max;

    /* Refcount — multiple opts (= multiple mounts) can hold the pool. */
    _Atomic int refcount;
};

/* ----- Cache layout ----- */

typedef struct AetherProxyCacheEntry {
    /* Doubly-linked list for LRU; head = most recently used. */
    struct AetherProxyCacheEntry *lru_prev, *lru_next;
    /* Hash bucket chain. */
    struct AetherProxyCacheEntry *bucket_next;

    uint64_t key_hash;
    char*    key_repr;                /* canonical key string for collision check */

    int    status_code;
    char** header_keys;
    char** header_values;
    int    header_count;
    char*  body;
    int    body_length;

    long   stored_at_ms;
    long   expires_at_ms;
    char*  etag;                      /* ETag value (without quotes) for IMS revalidation */
    char*  last_modified;             /* Last-Modified header value */

    /* Recorded Vary header from the upstream response. NULL when
     * the upstream emitted no Vary (or when the cache key strategy
     * isn't METHOD_URL_VARY). Used at lookup time to recompute the
     * request's vary-key suffix before comparing against key_repr. */
    char*  vary_header;
} AetherProxyCacheEntry;

struct AetherProxyCache {
    int max_entries;
    int max_body_bytes;
    int default_ttl_sec;
    AetherProxyCacheKeyStrategy key_strategy;

    pthread_mutex_t lock;
    AetherProxyCacheEntry** buckets;
    int    bucket_count;              /* always power-of-two */
    int    entry_count;
    AetherProxyCacheEntry *lru_head, *lru_tail;
};

/* ----- Per-mount options ----- */

struct AetherProxyOpts {
    char* path_prefix;                /* set by proxy.mount */
    char* route_pattern;              /* optional full-path pattern */
    char* methods;                    /* optional CSV allow-list */
    char* strip_path_prefix;
    int   preserve_host;
    int   add_xff;
    int   add_xfp;
    int   add_xfh;
    int   max_body_bytes;
    AetherProxyPool*  pool;           /* refcount-incremented on bind */
    AetherProxyCache* cache;          /* may be NULL */

    /* Idempotent retry policy. When max_retries > 0 the middleware
     * retries failed upstream calls (5xx + transport errors) for
     * idempotent methods (GET, HEAD, PUT, DELETE, OPTIONS) using
     * exponential backoff starting at backoff_base_ms with full
     * jitter (uniform random in [0, current_backoff]). Non-idempotent
     * methods (POST, PATCH) are NEVER retried — at-most-once
     * delivery is preserved. Zero disables retry. */
    int   retry_max_retries;
    int   retry_backoff_base_ms;

    /* Per-mount Trace-Context propagation.
     * 0 = pass through whatever the client sent (default — preserves
     *     end-to-end tracing when the client is trace-aware).
     * 1 = if no inbound traceparent, generate a new trace-id and
     *     stamp `traceparent: 00-<trace_id>-<span_id>-01` outbound. */
    int   trace_context_inject;
};

/* ----- Cross-file helpers ----- */

/* Monotonic milliseconds since some fixed epoch. clock_gettime(MONOTONIC)
 * on POSIX, QueryPerformanceCounter on Windows. */
long aether_proxy_now_ms(void);

/* Hold/release a pool refcount. */
void aether_proxy_pool_retain(AetherProxyPool* pool);

/* LB picker — defined in aether_proxy_lb.c. Returns NULL when no
 * eligible upstream exists (every healthy upstream is over the
 * inflight cap or has its breaker open). The returned upstream
 * has its inflight counter incremented; the caller must
 * `aether_proxy_inflight_dec(u)` after the upstream call returns. */
AetherUpstream* aether_proxy_lb_pick(AetherProxyPool* pool, HttpRequest* req);
void aether_proxy_inflight_dec(AetherUpstream* u);

/* Breaker — defined in aether_proxy_breaker.c. */
int  aether_proxy_breaker_admit(AetherProxyPool* pool,
                                AetherUpstream* u,
                                long now_ms);
void aether_proxy_breaker_record(AetherProxyPool* pool,
                                 AetherUpstream* u,
                                 int ok);

/* Cache — defined in aether_proxy_cache.c. Lookup returns the
 * stored entry on hit (caller must NOT free; the cache owns it),
 * or NULL on miss. The body+headers are written back into `res`
 * when populating from a cache hit. */
AetherProxyCacheEntry* aether_proxy_cache_lookup(AetherProxyCache* cache,
                                                 const char* method,
                                                 const char* url,
                                                 HttpRequest* req);
void aether_proxy_cache_store(AetherProxyCache* cache,
                              const char* method,
                              const char* url,
                              HttpRequest* req,
                              int status_code,
                              const char* response_headers,
                              const char* body,
                              int body_length);

#ifdef __cplusplus
}
#endif


/* One proxied request, suspendable at its single I/O point.
 *
 * The blocking server path and any caller that cannot block share these
 * steps, so the proxy's semantics (retries, breaker accounting, cache,
 * header rewriting) exist once rather than once per driver.
 */
#define PX_NEED_SEND 2

typedef struct {
    AetherProxyOpts*    opts;
    HttpRequest*        req;
    HttpServerResponse* res;
    AetherUpstream*     u;
    const char*         forward_path;
    HttpClientRequest*  outbound;      /* built, the caller sends it */
    char*               upstream_url;  /* owned alongside `outbound` */
    HttpResponse*       resp;          /* the caller stores the result here */
    int                 status;
    int                 attempt;
    int                 max_attempts;
} AetherProxyExchange;

/* Returns 0 or 1, the middleware's own result, when the exchange is finished,
 * or PX_NEED_SEND when `outbound` is built and waiting. On PX_NEED_SEND the
 * caller sends it, frees it and `upstream_url`, stores the reply in `resp`,
 * and calls resume with how long the send took in milliseconds. */
int aether_proxy_exchange_begin(AetherProxyExchange* px,
                                HttpRequest* req,
                                HttpServerResponse* res,
                                void* user_data);
int aether_proxy_exchange_resume(AetherProxyExchange* px, long elapsed_ms);

#endif  /* AETHER_PROXY_INTERNAL_H */
