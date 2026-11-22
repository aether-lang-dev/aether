/* aether_proxy_lb.c — load-balancer pickers for the proxy pool.
 *
 * Four algorithms; the active one is set on the pool at creation
 * (pool->algo) and never changes after.
 *
 *   ROUND_ROBIN   atomic counter mod N. Skips ineligible upstreams
 *                 by retrying up to N times.
 *   LEAST_CONN    iterate, pick the eligible upstream with smallest
 *                 atomic_load(&u->inflight). O(N) but N is small.
 *   IP_HASH       FNV-1a over client IP, mod N. Falls back to RR
 *                 when picked is ineligible (soft sticky).
 *   WEIGHTED_RR   smooth weighted RR (the algorithm nginx uses):
 *                 each upstream's current_weight += effective_weight;
 *                 pick max current_weight; subtract total_effective
 *                 from picked's current_weight. Produces interleaved
 *                 sequences (3:1 → A,A,A,B,A,A,A,B…) instead of
 *                 batched (AAAB AAAB).
 *
 * "Eligible" means: healthy && breaker not OPEN && inflight <
 * pool->max_inflight_per_up (when capped). The picker calls
 * `aether_proxy_breaker_admit` which handles the OPEN→HALF_OPEN
 * timeout transition; HALF_OPEN admits up to half_open_max
 * concurrent test requests.
 */

#include "aether_proxy_internal.h"
#if defined(AETHER_NO_THREADING)
#include "../../../runtime/actors/aether_panic.h"   /* aether_context_local: a cooperative build's per-task state */
#endif

#include <string.h>
#include <stdint.h>

/* ----- helpers ----- */

/* Token-bucket admit. Refills at rl_max_rps tokens/sec since last
 * refill, capped at rl_burst, then deducts one token. Returns 1
 * if admitted, 0 if the bucket is empty. Disabled when
 * rl_max_rps == 0. */
static int rate_limit_admit_locked(AetherUpstream* u, int64_t now_ms) {
    if (u->rl_max_rps == 0) return 1;
    int64_t elapsed = now_ms - u->rl_last_refill_ms;
    if (elapsed > 0) {
        double refill = (double)elapsed * (double)u->rl_max_rps / 1000.0;
        u->rl_tokens += refill;
        if (u->rl_tokens > (double)u->rl_burst) u->rl_tokens = (double)u->rl_burst;
        u->rl_last_refill_ms = now_ms;
    }
    if (u->rl_tokens >= 1.0) {
        u->rl_tokens -= 1.0;
        return 1;
    }
    return 0;
}

static int upstream_eligible(AetherProxyPool* pool, AetherUpstream* u, int64_t now_ms) {
    if (atomic_load(&u->draining)) return 0;
    if (!atomic_load(&u->healthy)) return 0;
    if (pool->max_inflight_per_up > 0 &&
        atomic_load(&u->inflight) >= pool->max_inflight_per_up) return 0;
    if (!aether_proxy_breaker_admit(pool, u, now_ms)) return 0;

    /* Rate limit: held briefly. Refill+deduct is one mutex-guarded
     * operation per pick; uncontended in steady state. When the
     * bucket is empty the upstream is treated as ineligible (the
     * picker moves to the next candidate). */
    if (u->rl_max_rps > 0) {
        pthread_mutex_lock(&u->rl_lock);
        int ok = rate_limit_admit_locked(u, now_ms);
        pthread_mutex_unlock(&u->rl_lock);
        if (!ok) return 0;
    }
    return 1;
}

static AetherUpstream* claim(AetherUpstream* u) {
    /* Mark in-flight before returning so concurrent pickers see the
     * load. Caller must release via aether_proxy_inflight_dec. */
    atomic_fetch_add(&u->inflight, 1);
    return u;
}

/* FNV-1a 64-bit. Used by IP_HASH for client identity hashing — not
 * cryptographic; just a stable, fast spread. */
static uint64_t fnv1a_64(const char* s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    if (!s) return h;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Look up a named cookie's value from the request's `Cookie:`
 * header. Returns a thread-local buffer pointer (lifetime: until
 * the next call on this thread). NULL when the cookie is absent
 * or the request has no `Cookie:` header. */
static const char* cookie_value(HttpRequest* req, const char* name) {
    if (!name || !*name) return NULL;
    const char* hdr = http_get_header(req, "Cookie");
    if (!hdr || !*hdr) return NULL;
#if defined(AETHER_NO_THREADING)
    /* A cooperative build: a block of the calling task's execution context
     * (aether_context_local), not a thread-local on every task's stack. */
    static const char buf_key = 0;
    char (*buf_block)[256] = (char (*)[256])aether_context_local(&buf_key, 256, NULL);
    if (!buf_block) return NULL;
#define buf (*buf_block)
#else
    static __thread char buf[256];
#endif

    size_t name_len = strlen(name);
    const char* p = hdr;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char* eq = strchr(p, '=');
        if (!eq) break;
        size_t key_len = (size_t)(eq - p);
        while (key_len > 0 && (p[key_len-1] == ' ' || p[key_len-1] == '\t')) key_len--;

        const char* vs = eq + 1;
        const char* ve = strchr(vs, ';');
        if (!ve) ve = vs + strlen(vs);
        if (key_len == name_len && strncmp(p, name, name_len) == 0) {
            /* Strip surrounding quotes if present. */
            if (ve > vs && *vs == '"' && ve[-1] == '"') { vs++; ve--; }
            size_t vl = (size_t)(ve - vs);
            if (vl >= sizeof(buf)) vl = sizeof(buf) - 1;
            memcpy(buf, vs, vl);
            buf[vl] = '\0';
            return buf;
        }
        p = (*ve == ';') ? ve + 1 : ve;
    }
    return NULL;
}
#undef buf

/* The client identity IP_HASH hashes, the rate limiter's (#3173): the
 * peer the request reports, the connection's or, after use_real_ip, the
 * client it resolved (#3169). Never a header: keyed on X-Forwarded-For
 * or X-Real-IP, a client picked its upstream by what it sent. A request
 * with no IP peer hashes as "anonymous". */
static const char* client_identity(HttpRequest* req) {
    const char* peer = http_request_remote_addr(req);
    return (peer && *peer) ? peer : "anonymous";
}

/* ----- per-algorithm pickers ----- */

static AetherUpstream* pick_round_robin(AetherProxyPool* pool, int64_t now_ms) {
    int n = pool->upstream_count;
    if (n == 0) return NULL;
    for (int tries = 0; tries < n; tries++) {
        int i = (int)((unsigned int)atomic_fetch_add(&pool->rr_cursor, 1) % (unsigned int)n);
        AetherUpstream* u = pool->upstreams[i];
        if (upstream_eligible(pool, u, now_ms)) return claim(u);
    }
    return NULL;
}

static AetherUpstream* pick_least_conn(AetherProxyPool* pool, int64_t now_ms) {
    int n = pool->upstream_count;
    if (n == 0) return NULL;
    AetherUpstream* best = NULL;
    int best_inflight = 0;
    for (int i = 0; i < n; i++) {
        AetherUpstream* u = pool->upstreams[i];
        if (!upstream_eligible(pool, u, now_ms)) continue;
        int cur = atomic_load(&u->inflight);
        if (!best || cur < best_inflight) {
            best = u;
            best_inflight = cur;
        }
    }
    return best ? claim(best) : NULL;
}

static AetherUpstream* pick_ip_hash(AetherProxyPool* pool, HttpRequest* req, int64_t now_ms) {
    int n = pool->upstream_count;
    if (n == 0) return NULL;
    uint64_t h = fnv1a_64(client_identity(req));
    int start = (int)(h % (unsigned int)n);
    AetherUpstream* picked = pool->upstreams[start];
    if (upstream_eligible(pool, picked, now_ms)) return claim(picked);

    /* Soft sticky: when the natural pick is ineligible, fall back to
     * RR-like progress so the request goes somewhere. Documented. */
    for (int tries = 1; tries < n; tries++) {
        AetherUpstream* u = pool->upstreams[(start + tries) % n];
        if (upstream_eligible(pool, u, now_ms)) return claim(u);
    }
    return NULL;
}

/* Cookie-hash sticky sessions. Hash the configured cookie's value
 * (FNV-1a) to pick an upstream. Falls back to RR when the cookie
 * is absent OR when the natural pick is ineligible (mirrors the
 * IP_HASH soft-sticky semantics so requests still go somewhere
 * during partial outages). */
static AetherUpstream* pick_cookie_hash(AetherProxyPool* pool, HttpRequest* req, int64_t now_ms) {
    int n = pool->upstream_count;
    if (n == 0) return NULL;
    if (!pool->cookie_name) {
        /* Misconfigured — caller didn't set a cookie name. RR fallback. */
        return pick_round_robin(pool, now_ms);
    }
    const char* val = cookie_value(req, pool->cookie_name);
    if (!val) {
        /* Cookie absent on this request — pick any eligible upstream
         * via RR so first-request-from-a-new-client still works. The
         * client's response can carry a Set-Cookie that pins them to
         * an upstream for future requests. */
        return pick_round_robin(pool, now_ms);
    }
    uint64_t h = fnv1a_64(val);
    int start = (int)(h % (unsigned int)n);
    AetherUpstream* picked = pool->upstreams[start];
    if (upstream_eligible(pool, picked, now_ms)) return claim(picked);
    for (int tries = 1; tries < n; tries++) {
        AetherUpstream* u = pool->upstreams[(start + tries) % n];
        if (upstream_eligible(pool, u, now_ms)) return claim(u);
    }
    return NULL;
}

static AetherUpstream* pick_weighted_rr(AetherProxyPool* pool, int64_t now_ms) {
    /* Smooth weighted RR (nginx's algorithm).
     *
     * For each upstream:    current_weight += effective_weight
     * Pick the upstream with the largest current_weight.
     * Subtract total_effective_weight from picked.current_weight.
     *
     * Produces interleaved sequences for non-trivial weights and
     * recovers gracefully when an upstream fails (effective_weight
     * shrinks toward 0; recovers additively per success). */

    int n = pool->upstream_count;
    if (n == 0) return NULL;

    AetherUpstream* best = NULL;
    int total = 0;

    for (int i = 0; i < n; i++) {
        AetherUpstream* u = pool->upstreams[i];
        if (!upstream_eligible(pool, u, now_ms)) continue;

        u->current_weight += u->effective_weight;
        total += u->effective_weight;

        if (!best || u->current_weight > best->current_weight) {
            best = u;
        }
    }

    if (!best) return NULL;
    best->current_weight -= total;
    return claim(best);
}

/* ----- public picker ----- */

AetherUpstream* aether_proxy_lb_pick(AetherProxyPool* pool, HttpRequest* req) {
    if (!pool) return NULL;
    int64_t now_ms = aether_proxy_now_ms();

    pthread_mutex_lock(&pool->lock);
    AetherUpstream* u = NULL;
    switch (pool->algo) {
    case AETHER_PROXY_LB_ROUND_ROBIN:
        u = pick_round_robin(pool, now_ms);
        break;
    case AETHER_PROXY_LB_LEAST_CONN:
        u = pick_least_conn(pool, now_ms);
        break;
    case AETHER_PROXY_LB_IP_HASH:
        u = pick_ip_hash(pool, req, now_ms);
        break;
    case AETHER_PROXY_LB_WEIGHTED_RR:
        u = pick_weighted_rr(pool, now_ms);
        break;
    case AETHER_PROXY_LB_COOKIE_HASH:
        u = pick_cookie_hash(pool, req, now_ms);
        break;
    }
    pthread_mutex_unlock(&pool->lock);
    return u;
}
