/* aether_proxy_metrics.c — Prometheus-format metrics rendering
 * for the proxy pool.
 *
 * Reads the `_Atomic` per-upstream counters (no pool lock needed
 * for those — atomic loads are lock-free) plus the pool-level
 * counters, snapshots them into a malloc'd text buffer in
 * Prometheus 0.0.4 exposition format. Caller frees the returned
 * string.
 *
 * The buffer grows dynamically because the size is bounded by
 * upstream count × per-upstream-line-bytes; we don't predict
 * exactly so a grow-on-demand pattern is the simplest correct
 * shape.
 *
 * Output format (one HELP + TYPE block per metric, then samples
 * with `upstream` label per upstream):
 *
 *   # HELP aether_proxy_upstream_requests_total ...
 *   # TYPE aether_proxy_upstream_requests_total counter
 *   aether_proxy_upstream_requests_total{upstream="http://x:8080",class="2xx"} 42
 *   ...
 */

#include "aether_proxy_internal.h"
#include "../../mem/aether_grow.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Dynamic string buffer. */
typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} TextBuf;

static int tb_reserve(TextBuf* b, size_t need) {
    if (b->len + need + 1 <= b->cap) return 0;
    size_t cap = aether_buf_grow_capacity(b->cap, 4096, b->len + need + 1, 1);
    if (!cap) return -1;
    char* g = (char*)realloc(b->data, cap);
    if (!g) return -1;
    b->data = g;
    b->cap = cap;
    return 0;
}

static int tb_print(TextBuf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static int tb_print(TextBuf* b, const char* fmt, ...) {
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(stack)) {
        /* Rare path — vary-large render. Fall back to malloc. */
        char* big = (char*)malloc((size_t)n + 1);
        if (!big) return -1;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        if (tb_reserve(b, (size_t)n) < 0) { free(big); return -1; }
        memcpy(b->data + b->len, big, (size_t)n);
        b->len += (size_t)n;
        b->data[b->len] = '\0';
        free(big);
        return 0;
    }
    if (tb_reserve(b, (size_t)n) < 0) return -1;
    memcpy(b->data + b->len, stack, (size_t)n);
    b->len += (size_t)n;
    b->data[b->len] = '\0';
    return 0;
}

/* Escape `s` for use as a Prometheus label-value (RFC: replace
 * backslash, double-quote, and newline). Returns malloc'd string;
 * caller frees. */
static char* prom_label_escape(const char* s) {
    if (!s) s = "";
    size_t n = strlen(s);
    /* Worst case 2x. */
    char* out = (char*)malloc(2 * n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' || c == '"') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n')         { out[j++] = '\\'; out[j++] = 'n'; }
        else                         { out[j++] = c; }
    }
    out[j] = '\0';
    return out;
}

#define EMIT(...) do { if (tb_print(&buf, __VA_ARGS__) < 0) goto oom; } while (0)

char* aether_proxy_pool_metrics_text(AetherProxyPool* pool) {
    if (!pool) return NULL;
    TextBuf buf = {0};

    /* Snapshot upstream list under the pool lock so we don't race
     * with add/remove. We hold the lock briefly — atomics off the
     * snapshot don't need it. */
    pthread_mutex_lock(&pool->lock);
    int n = pool->upstream_count;
    AetherUpstream** snap = NULL;
    if (n > 0) {
        snap = (AetherUpstream**)malloc((size_t)n * sizeof(AetherUpstream*));
        if (!snap) { pthread_mutex_unlock(&pool->lock); return NULL; }
        for (int i = 0; i < n; i++) snap[i] = pool->upstreams[i];
    }
    pthread_mutex_unlock(&pool->lock);

    /* Single escape scratch pointer at function scope. The per-loop
     * declarations left a live `esc` leaked whenever an EMIT hit OOM and
     * jumped to `oom:` before its free; a hoisted pointer (NULLed after each
     * free) lets the oom path reclaim it. */
    char* esc = NULL;

    /* requests_total — broken down by status class so a Grafana
     * dashboard can sum {class="5xx"} for an error-rate panel
     * without re-aggregating. */
    EMIT("# HELP aether_proxy_upstream_requests_total Total proxied requests by upstream and HTTP status class.\n");
    EMIT("# TYPE aether_proxy_upstream_requests_total counter\n");
    for (int i = 0; i < n; i++) {
        AetherUpstream* u = snap[i];
        esc = prom_label_escape(u->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_requests_total{upstream=\"%s\",class=\"2xx\"} %ld\n", esc, atomic_load(&u->metric_requests_2xx));
        EMIT("aether_proxy_upstream_requests_total{upstream=\"%s\",class=\"3xx\"} %ld\n", esc, atomic_load(&u->metric_requests_3xx));
        EMIT("aether_proxy_upstream_requests_total{upstream=\"%s\",class=\"4xx\"} %ld\n", esc, atomic_load(&u->metric_requests_4xx));
        EMIT("aether_proxy_upstream_requests_total{upstream=\"%s\",class=\"5xx\"} %ld\n", esc, atomic_load(&u->metric_requests_5xx));
        free(esc); esc = NULL;
    }

    EMIT("# HELP aether_proxy_upstream_transport_errors_total Transport-class failures (DNS, connect, TLS).\n");
    EMIT("# TYPE aether_proxy_upstream_transport_errors_total counter\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_transport_errors_total{upstream=\"%s\"} %ld\n", esc, atomic_load(&snap[i]->metric_transport_errors));
        free(esc); esc = NULL;
    }

    EMIT("# HELP aether_proxy_upstream_timeouts_total Upstream calls that exceeded the request timeout.\n");
    EMIT("# TYPE aether_proxy_upstream_timeouts_total counter\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_timeouts_total{upstream=\"%s\"} %ld\n", esc, atomic_load(&snap[i]->metric_timeouts));
        free(esc); esc = NULL;
    }

    EMIT("# HELP aether_proxy_upstream_retries_total Idempotent retries fired against this upstream.\n");
    EMIT("# TYPE aether_proxy_upstream_retries_total counter\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_retries_total{upstream=\"%s\"} %ld\n", esc, atomic_load(&snap[i]->metric_retries));
        free(esc); esc = NULL;
    }

    /* Latency: emit sum + count so dashboards can compute average
     * (sum / count) without histograms. v2 will swap in real
     * histogram buckets. */
    EMIT("# HELP aether_proxy_upstream_latency_ms_sum Sum of upstream call durations in ms.\n");
    EMIT("# TYPE aether_proxy_upstream_latency_ms_sum counter\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_latency_ms_sum{upstream=\"%s\"} %ld\n", esc, atomic_load(&snap[i]->metric_latency_sum_ms));
        free(esc); esc = NULL;
    }
    EMIT("# HELP aether_proxy_upstream_latency_ms_count Count of upstream calls observed.\n");
    EMIT("# TYPE aether_proxy_upstream_latency_ms_count counter\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_latency_ms_count{upstream=\"%s\"} %ld\n", esc, atomic_load(&snap[i]->metric_latency_count));
        free(esc); esc = NULL;
    }

    /* Gauges. */
    EMIT("# HELP aether_proxy_upstream_inflight Concurrent in-flight requests on this upstream.\n");
    EMIT("# TYPE aether_proxy_upstream_inflight gauge\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_inflight{upstream=\"%s\"} %d\n", esc, atomic_load(&snap[i]->inflight));
        free(esc); esc = NULL;
    }

    EMIT("# HELP aether_proxy_upstream_healthy 1 if the active health check considers the upstream up.\n");
    EMIT("# TYPE aether_proxy_upstream_healthy gauge\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_healthy{upstream=\"%s\"} %d\n", esc, atomic_load(&snap[i]->healthy));
        free(esc); esc = NULL;
    }

    EMIT("# HELP aether_proxy_upstream_breaker_state 0=closed, 1=open, 2=half_open.\n");
    EMIT("# TYPE aether_proxy_upstream_breaker_state gauge\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_breaker_state{upstream=\"%s\"} %d\n", esc, atomic_load(&snap[i]->cb_state));
        free(esc); esc = NULL;
    }

    EMIT("# HELP aether_proxy_upstream_draining 1 if the upstream is in active drain mode.\n");
    EMIT("# TYPE aether_proxy_upstream_draining gauge\n");
    for (int i = 0; i < n; i++) {
        esc = prom_label_escape(snap[i]->base_url);
        if (!esc) goto oom;
        EMIT("aether_proxy_upstream_draining{upstream=\"%s\"} %d\n", esc, atomic_load(&snap[i]->draining));
        free(esc); esc = NULL;
    }

    /* Pool-level counters. */
    EMIT("# HELP aether_proxy_cache_hits_total Cache hits served from the proxy LRU.\n");
    EMIT("# TYPE aether_proxy_cache_hits_total counter\n");
    EMIT("aether_proxy_cache_hits_total %ld\n", atomic_load(&pool->metric_cache_hits));
    EMIT("# HELP aether_proxy_cache_misses_total Cache misses; an upstream call was made.\n");
    EMIT("# TYPE aether_proxy_cache_misses_total counter\n");
    EMIT("aether_proxy_cache_misses_total %ld\n", atomic_load(&pool->metric_cache_misses));
    EMIT("# HELP aether_proxy_cache_revalidations_total Conditional GET revalidations against upstream.\n");
    EMIT("# TYPE aether_proxy_cache_revalidations_total counter\n");
    EMIT("aether_proxy_cache_revalidations_total %ld\n", atomic_load(&pool->metric_cache_revalidations));
    EMIT("# HELP aether_proxy_503_no_upstream_total 503 responses emitted because no upstream was eligible.\n");
    EMIT("# TYPE aether_proxy_503_no_upstream_total counter\n");
    EMIT("aether_proxy_503_no_upstream_total %ld\n", atomic_load(&pool->metric_503_no_upstream));

    free(snap);
    if (buf.data == NULL) {
        /* No metrics emitted at all (empty pool, no allocations). */
        buf.data = strdup("# Aether proxy: empty pool, no metrics.\n");
    }
    return buf.data;

oom:
    free(esc);
    free(snap);
    free(buf.data);
    return NULL;
}

#undef EMIT
