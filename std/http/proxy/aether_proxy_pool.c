/* aether_proxy_pool.c — AetherUpstream + AetherProxyPool lifecycle.
 *
 * Pool ownership model: `aether_proxy_pool_new` returns a pool with
 * refcount=1. Each AetherProxyOpts that binds to the pool calls
 * `aether_proxy_pool_retain`; opts_free releases. The public
 * `aether_proxy_pool_free` decrements; on zero, joins the
 * health-check thread and frees the upstream array.
 *
 * Thread safety: add/remove + WRR running state guarded by
 * pool->lock. LB picker reads under the same lock for upstream
 * array stability; per-upstream counters (healthy / inflight /
 * breaker) are atomics so the picker doesn't have to take the
 * lock for them.
 */

#include "aether_proxy_internal.h"
#include "../../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../../runtime/utils/aether_thread.h"
#include "../../net/aether_http.h"

/* ----- helpers exposed to other proxy .c files ----- */

long aether_proxy_now_ms(void) {
    /* Only differences matter here (health-check and breaker timings).
     * Delegates to the shared monotonic clock so Windows uses the
     * overflow-free QueryPerformanceCounter split in one place. */
    return (long)http_clock_ms();
}

/* ----- LB algo string mapping ----- */

int aether_proxy_lb_algo_from_string(const char* name) {
    if (!name) return -1;
    if (strcmp(name, "round_robin") == 0) return AETHER_PROXY_LB_ROUND_ROBIN;
    if (strcmp(name, "least_conn")  == 0) return AETHER_PROXY_LB_LEAST_CONN;
    if (strcmp(name, "ip_hash")     == 0) return AETHER_PROXY_LB_IP_HASH;
    if (strcmp(name, "weighted_rr") == 0) return AETHER_PROXY_LB_WEIGHTED_RR;
    if (strcmp(name, "cookie_hash") == 0) return AETHER_PROXY_LB_COOKIE_HASH;
    return -1;
}

/* ----- AetherUpstream lifecycle ----- */

static AetherUpstream* upstream_new(const char* base_url, int weight) {
    AetherUpstream* u = (AetherUpstream*)aether_caps_calloc(1, sizeof(AetherUpstream));
    if (!u) return NULL;
    u->base_url = strdup(base_url);
    if (!u->base_url) { aether_caps_free(u, sizeof(AetherUpstream)); return NULL; }
    /* The Host: header the proxy rewrites to is this slice of base_url, and
     * it was recomputed on every forwarded request. */
    u->authority = aether_proxy_authority_of(base_url);
    if (!u->authority) {
        free(u->base_url);
        aether_caps_free(u, sizeof(AetherUpstream));
        return NULL;
    }
    u->weight           = weight > 0 ? weight : 1;
    u->effective_weight = u->weight;
    u->current_weight   = 0;
    atomic_init(&u->healthy, 1);
    atomic_init(&u->consecutive_ok, 0);
    atomic_init(&u->consecutive_fail, 0);
    atomic_init(&u->draining, 0);
    atomic_init(&u->inflight, 0);
    atomic_init(&u->cb_state, AETHER_PROXY_CB_CLOSED);
    atomic_init(&u->cb_consecutive_failures, 0);
    atomic_init(&u->cb_opened_at_ms, 0);
    atomic_init(&u->cb_half_open_inflight, 0);
    pthread_mutex_init(&u->rl_lock, NULL);
    u->rl_max_rps        = 0;     /* disabled by default */
    u->rl_burst          = 0;
    u->rl_tokens         = 0.0;
    u->rl_last_refill_ms = 0;
    atomic_init(&u->metric_requests_2xx, 0);
    atomic_init(&u->metric_requests_3xx, 0);
    atomic_init(&u->metric_requests_4xx, 0);
    atomic_init(&u->metric_requests_5xx, 0);
    atomic_init(&u->metric_transport_errors, 0);
    atomic_init(&u->metric_timeouts, 0);
    atomic_init(&u->metric_retries, 0);
    atomic_init(&u->metric_latency_sum_ms, 0);
    atomic_init(&u->metric_latency_count, 0);
    return u;
}

static void upstream_free(AetherUpstream* u) {
    if (!u) return;
    pthread_mutex_destroy(&u->rl_lock);
    free(u->authority);
    free(u->base_url);
    aether_caps_free(u, sizeof(AetherUpstream));
}

void aether_proxy_inflight_dec(AetherUpstream* u) {
    if (!u) return;
    atomic_fetch_sub(&u->inflight, 1);
}

/* ----- Pool lifecycle ----- */

AetherProxyPool* aether_proxy_pool_new(AetherProxyLbAlgo algo,
                                       int request_timeout_sec,
                                       int dial_timeout_ms,
                                       int max_inflight_per_up) {
    if (algo < 0 || algo > AETHER_PROXY_LB_COOKIE_HASH) return NULL;
    if (request_timeout_sec < 0)  return NULL;
    if (dial_timeout_ms < 0)      return NULL;
    if (max_inflight_per_up < 0)  return NULL;

    AetherProxyPool* p = (AetherProxyPool*)aether_caps_calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->algo                  = algo;
    p->request_timeout_sec   = request_timeout_sec;
    p->dial_timeout_ms       = dial_timeout_ms;
    p->max_inflight_per_up   = max_inflight_per_up;

    pthread_mutex_init(&p->lock, NULL);
    pthread_mutex_init(&p->hc_cv_lock, NULL);
    pthread_cond_init(&p->hc_cv, NULL);
    atomic_init(&p->rr_cursor, 0);
    atomic_init(&p->hc_stopping, 0);
    atomic_init(&p->refcount, 1);

    /* Breaker disabled by default — caller opts in via configure. */
    p->br_failure_threshold = 0;
    p->br_open_duration_ms  = 30000;
    p->br_half_open_max     = 1;

    p->cookie_name = NULL;
    atomic_init(&p->metric_cache_hits, 0);
    atomic_init(&p->metric_cache_misses, 0);
    atomic_init(&p->metric_cache_revalidations, 0);
    atomic_init(&p->metric_503_no_upstream, 0);

    return p;
}

void aether_proxy_pool_retain(AetherProxyPool* pool) {
    if (!pool) return;
    atomic_fetch_add(&pool->refcount, 1);
}

void aether_proxy_pool_free(AetherProxyPool* pool) {
    if (!pool) return;

    int prev = atomic_fetch_sub(&pool->refcount, 1);
    if (prev > 1) return;  /* still other holders */

    /* Stop + join the health-check thread before tearing down
     * upstream state. The thread may be sleeping on hc_cv with a
     * timed wait; signalling wakes it instantly. */
    if (pool->hc_started) {
        atomic_store(&pool->hc_stopping, 1);
        pthread_mutex_lock(&pool->hc_cv_lock);
        pthread_cond_broadcast(&pool->hc_cv);
        pthread_mutex_unlock(&pool->hc_cv_lock);
        pthread_join(pool->hc_thread, NULL);
        pool->hc_started = 0;
    }

    /* Free upstreams. */
    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->upstream_count; i++) {
        upstream_free(pool->upstreams[i]);
    }
    free(pool->upstreams);
    pool->upstreams = NULL;
    pool->upstream_count = 0;
    pool->upstream_cap = 0;
    pthread_mutex_unlock(&pool->lock);

    free(pool->hc_probe_path);
    free(pool->cookie_name);
    pthread_mutex_destroy(&pool->lock);
    pthread_mutex_destroy(&pool->hc_cv_lock);
    pthread_cond_destroy(&pool->hc_cv);
    aether_caps_free(pool, sizeof(*pool));
}

/* ----- Drain ----- */

static AetherUpstream* find_upstream_locked(AetherProxyPool* pool,
                                            const char* base_url) {
    for (int i = 0; i < pool->upstream_count; i++) {
        if (strcmp(pool->upstreams[i]->base_url, base_url) == 0) {
            return pool->upstreams[i];
        }
    }
    return NULL;
}

static const char* set_drain(AetherProxyPool* pool, const char* base_url, int v) {
    if (!pool) return "pool is null";
    if (!base_url) return "base_url is null";
    pthread_mutex_lock(&pool->lock);
    AetherUpstream* u = find_upstream_locked(pool, base_url);
    if (!u) {
        pthread_mutex_unlock(&pool->lock);
        return "upstream not in pool";
    }
    atomic_store(&u->draining, v);
    pthread_mutex_unlock(&pool->lock);
    return "";
}

const char* aether_proxy_upstream_drain(AetherProxyPool* pool, const char* base_url) {
    return set_drain(pool, base_url, 1);
}
const char* aether_proxy_upstream_undrain(AetherProxyPool* pool, const char* base_url) {
    return set_drain(pool, base_url, 0);
}

/* ----- Per-upstream rate limit ----- */

const char* aether_proxy_rate_limit_set(AetherProxyPool* pool, int max_rps, int burst) {
    if (!pool) return "pool is null";
    if (max_rps < 0) return "max_rps must be >= 0";
    if (burst  < 0) return "burst must be >= 0";

    pthread_mutex_lock(&pool->lock);
    long now = aether_proxy_now_ms();
    for (int i = 0; i < pool->upstream_count; i++) {
        AetherUpstream* u = pool->upstreams[i];
        pthread_mutex_lock(&u->rl_lock);
        u->rl_max_rps = max_rps;
        u->rl_burst   = burst > 0 ? burst : max_rps;
        u->rl_tokens  = (double)u->rl_burst;
        u->rl_last_refill_ms = now;
        pthread_mutex_unlock(&u->rl_lock);
    }
    pthread_mutex_unlock(&pool->lock);
    return "";
}

const char* aether_proxy_pool_set_cookie_name(AetherProxyPool* pool,
                                              const char* cookie_name) {
    if (!pool) return "pool is null";
    if (!cookie_name || !*cookie_name) return "cookie_name is empty";
    pthread_mutex_lock(&pool->lock);
    free(pool->cookie_name);
    pool->cookie_name = strdup(cookie_name);
    int ok = (pool->cookie_name != NULL);
    pthread_mutex_unlock(&pool->lock);
    return ok ? "" : "out of memory";
}

/* ----- Upstream add/remove ----- */

const char* aether_proxy_upstream_add(AetherProxyPool* pool,
                                      const char* base_url,
                                      int weight) {
    if (!pool) return "pool is null";
    if (!base_url || !*base_url) return "base_url is empty";
    /* Quick scheme sanity check — the http client will fail at
     * send time anyway, but a clear error here is more useful. */
    if (strncmp(base_url, "http://",  7) != 0 &&
        strncmp(base_url, "https://", 8) != 0) {
        return "base_url must start with http:// or https://";
    }
    if (weight < 0) return "weight must be >= 0";

    pthread_mutex_lock(&pool->lock);

    /* Reject duplicates — the picker assumes each upstream is
     * uniquely identified by base_url. */
    for (int i = 0; i < pool->upstream_count; i++) {
        if (strcmp(pool->upstreams[i]->base_url, base_url) == 0) {
            pthread_mutex_unlock(&pool->lock);
            return "duplicate upstream URL";
        }
    }

    /* Grow the array. Modest N → linear realloc is fine. */
    if (pool->upstream_count == pool->upstream_cap) {
        int new_cap = pool->upstream_cap == 0 ? 4 : pool->upstream_cap * 2;
        AetherUpstream** grown = (AetherUpstream**)realloc(
            pool->upstreams, (size_t)new_cap * sizeof(AetherUpstream*));
        if (!grown) {
            pthread_mutex_unlock(&pool->lock);
            return "out of memory";
        }
        pool->upstreams = grown;
        pool->upstream_cap = new_cap;
    }

    AetherUpstream* u = upstream_new(base_url, weight);
    if (!u) {
        pthread_mutex_unlock(&pool->lock);
        return "out of memory";
    }

    /* Inherit pool-wide rate-limit config from any existing
     * upstream — ensures `rate_limit_set` then `upstream_add`
     * doesn't leave the new upstream unprotected. */
    if (pool->upstream_count > 0) {
        AetherUpstream* peer = pool->upstreams[0];
        pthread_mutex_lock(&peer->rl_lock);
        int max_rps = peer->rl_max_rps;
        int burst   = peer->rl_burst;
        pthread_mutex_unlock(&peer->rl_lock);
        if (max_rps > 0) {
            pthread_mutex_lock(&u->rl_lock);
            u->rl_max_rps = max_rps;
            u->rl_burst   = burst;
            u->rl_tokens  = (double)burst;
            u->rl_last_refill_ms = aether_proxy_now_ms();
            pthread_mutex_unlock(&u->rl_lock);
        }
    }

    pool->upstreams[pool->upstream_count++] = u;

    pthread_mutex_unlock(&pool->lock);
    return "";
}

const char* aether_proxy_upstream_remove(AetherProxyPool* pool,
                                         const char* base_url) {
    if (!pool) return "pool is null";
    if (!base_url) return "";
    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->upstream_count; i++) {
        if (strcmp(pool->upstreams[i]->base_url, base_url) == 0) {
            upstream_free(pool->upstreams[i]);
            for (int j = i; j < pool->upstream_count - 1; j++) {
                pool->upstreams[j] = pool->upstreams[j + 1];
            }
            pool->upstream_count--;
            pthread_mutex_unlock(&pool->lock);
            return "";
        }
    }
    pthread_mutex_unlock(&pool->lock);
    return "";  /* idempotent: not-found is success */
}
