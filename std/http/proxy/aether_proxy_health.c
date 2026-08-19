/* aether_proxy_health.c — active health-check thread.
 *
 * One pthread per pool (not per upstream — N-thread blow-up at
 * scale). The thread iterates the upstream list every interval_ms,
 * fires `GET probe_path` against each base_url via std.http.client,
 * tallies wins/losses, and flips `_Atomic int healthy` once the
 * threshold is crossed. healthy=0 makes the LB picker skip the
 * upstream until it recovers (consecutive_ok ≥ healthy_threshold).
 *
 * Sleeps on hc_cv with a timed wait so shutdown wakes instantly:
 *   pool_free sets hc_stopping=1, signals hc_cv, joins the thread.
 *
 * Threading via runtime/utils/aether_thread.h. The probe loop calls
 * into std.http.client which uses its own private global state
 * (DNS cache, OpenSSL CTX init); these are safe to call from the
 * health-check thread because they're already protected for the
 * server's primary forwarding traffic.
 */

#include "aether_proxy_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The std.http.client surface. Included directly now that the client and
 * server headers can coexist (#1433): this file used to hand-declare these
 * prototypes, which meant the compiler could not check them against the real
 * definitions. */
#include "../../net/aether_http.h"

/* Build "<base_url><probe_path>" into a freshly-malloc'd string.
 * Caller frees. Avoids double-slash by chopping a trailing '/' from
 * base_url when probe_path starts with '/'. */
static char* build_probe_url(const char* base_url, const char* probe_path) {
    if (!base_url) return NULL;
    if (!probe_path) probe_path = "/";

    size_t bl = strlen(base_url);
    size_t pl = strlen(probe_path);

    /* Strip trailing '/' from base if probe starts with '/'. */
    int chop = (bl > 0 && base_url[bl - 1] == '/' && pl > 0 && probe_path[0] == '/');
    size_t out_len = (chop ? bl - 1 : bl) + pl + 1;

    char* out = (char*)malloc(out_len);
    if (!out) return NULL;
    if (chop) {
        memcpy(out, base_url, bl - 1);
        memcpy(out + bl - 1, probe_path, pl);
        out[bl - 1 + pl] = '\0';
    } else {
        memcpy(out, base_url, bl);
        memcpy(out + bl, probe_path, pl);
        out[bl + pl] = '\0';
    }
    return out;
}

/* Probe a single upstream. Updates `consecutive_ok / consecutive_fail`
 * and flips `healthy` when the threshold trips. */
static void probe_one(AetherProxyPool* pool, AetherUpstream* u) {
    char* url = build_probe_url(u->base_url, pool->hc_probe_path);
    if (!url) return;

    HttpClientRequest* creq = http_request_raw("GET", url);
    int ok = 0;
    if (creq) {
        if (pool->hc_timeout_ms > 0) {
            int sec = (pool->hc_timeout_ms + 999) / 1000;
            if (sec < 1) sec = 1;
            http_request_set_timeout_raw(creq, sec);
        }
        HttpResponse* cresp = http_send_raw(creq);
        if (cresp) {
            const char* err = http_response_error(cresp);
            int status = http_response_status(cresp);
            if (!err || !*err) {
                if (pool->hc_expect_status == 0) {
                    ok = (status >= 200 && status < 300);
                } else {
                    ok = (status == pool->hc_expect_status);
                }
            }
            http_response_free(cresp);
        }
        http_request_free_raw(creq);
    }
    free(url);

    if (ok) {
        atomic_store(&u->consecutive_fail, 0);
        int n = atomic_fetch_add(&u->consecutive_ok, 1) + 1;
        if (!atomic_load(&u->healthy) && n >= pool->hc_healthy_threshold) {
            atomic_store(&u->healthy, 1);
        }
    } else {
        atomic_store(&u->consecutive_ok, 0);
        int n = atomic_fetch_add(&u->consecutive_fail, 1) + 1;
        if (atomic_load(&u->healthy) && n >= pool->hc_unhealthy_threshold) {
            atomic_store(&u->healthy, 0);
        }
    }
}

/* Main loop. Runs until hc_stopping is set. */
static void* health_check_loop(void* arg) {
    AetherProxyPool* pool = (AetherProxyPool*)arg;

    while (!atomic_load(&pool->hc_stopping)) {
        /* Snapshot the upstream list under the lock so we can probe
         * without holding it (probes hit the network and can take
         * timeout_ms). */
        pthread_mutex_lock(&pool->lock);
        int n = pool->upstream_count;
        AetherUpstream** snap = NULL;
        if (n > 0) {
            snap = (AetherUpstream**)malloc((size_t)n * sizeof(AetherUpstream*));
            if (snap) {
                for (int i = 0; i < n; i++) snap[i] = pool->upstreams[i];
            }
        }
        pthread_mutex_unlock(&pool->lock);

        if (snap) {
            for (int i = 0; i < n && !atomic_load(&pool->hc_stopping); i++) {
                probe_one(pool, snap[i]);
            }
            free(snap);
        }

        if (atomic_load(&pool->hc_stopping)) break;

        /* Sleep on hc_cv with timed wait — wakes instantly on shutdown. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += pool->hc_interval_ms / 1000;
        ts.tv_nsec += (pool->hc_interval_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

        pthread_mutex_lock(&pool->hc_cv_lock);
        if (!atomic_load(&pool->hc_stopping)) {
            pthread_cond_timedwait(&pool->hc_cv, &pool->hc_cv_lock, &ts);
        }
        pthread_mutex_unlock(&pool->hc_cv_lock);
    }
    return NULL;
}

const char* aether_proxy_health_checks_enable(AetherProxyPool* pool,
                                              const char* probe_path,
                                              int expect_status,
                                              int interval_ms,
                                              int timeout_ms,
                                              int healthy_threshold,
                                              int unhealthy_threshold) {
    if (!pool) return "pool is null";
    if (!probe_path || !*probe_path) return "probe_path must not be empty";
    if (expect_status < 0)            return "expect_status must be >= 0";
    if (interval_ms <= 0)             return "interval_ms must be > 0";
    if (timeout_ms <= 0)              return "timeout_ms must be > 0";
    if (healthy_threshold <= 0)       return "healthy_threshold must be > 0";
    if (unhealthy_threshold <= 0)     return "unhealthy_threshold must be > 0";

    /* Reconfigure: stop the existing thread first so we don't end
     * up with two probe loops on the same pool. */
    if (pool->hc_started) {
        atomic_store(&pool->hc_stopping, 1);
        pthread_mutex_lock(&pool->hc_cv_lock);
        pthread_cond_broadcast(&pool->hc_cv);
        pthread_mutex_unlock(&pool->hc_cv_lock);
        pthread_join(pool->hc_thread, NULL);
        pool->hc_started = 0;
        atomic_store(&pool->hc_stopping, 0);
        free(pool->hc_probe_path);
        pool->hc_probe_path = NULL;
    }

    pool->hc_probe_path        = strdup(probe_path);
    if (!pool->hc_probe_path) return "out of memory";
    pool->hc_expect_status     = expect_status;
    pool->hc_interval_ms       = interval_ms;
    pool->hc_timeout_ms        = timeout_ms;
    pool->hc_healthy_threshold = healthy_threshold;
    pool->hc_unhealthy_threshold = unhealthy_threshold;

#if !AETHER_HAS_THREADS
    /* Active health checking is a background thread by definition, so there is
     * nothing to degrade to on a threadless target (wasi, Emscripten). Report
     * it rather than silently returning success and leaving the caller to
     * believe upstreams are being probed — a pool that reports healthy without
     * checking is worse than one that says it cannot check.
     *
     * This is a call-site guard rather than a pthread_create stub in
     * aether_thread.h: reaching a create on a threadless platform IS a logic
     * error, and stubbing it would let every other such site fail silently. */
    free(pool->hc_probe_path);
    pool->hc_probe_path = NULL;
    return "active health checks need threads (unavailable on this target)";
#else
    if (pthread_create(&pool->hc_thread, NULL, health_check_loop, pool) != 0) {
        free(pool->hc_probe_path);
        pool->hc_probe_path = NULL;
        return "failed to spawn health-check thread";
    }
    pool->hc_started = 1;
    return "";
#endif
}
