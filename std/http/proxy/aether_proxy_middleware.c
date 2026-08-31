/* aether_proxy_middleware.c — the proxy middleware glue.
 *
 * Flow per request:
 *   1. Path-prefix match. If no match, return 1 (continue chain).
 *   2. Body cap check; oversize → 502 "request body exceeds proxy cap".
 *   3. Optional cache lookup (GET/HEAD only). On hit → write cached
 *      response into `res`, return 0.
 *   4. LB pick. NULL → 503 "no upstream available".
 *   5. Build the outbound request via std.http.client:
 *        - method + (upstream_base + req->path[strip_prefix..])
 *        - copy inbound headers minus Hop-by-Hop (RFC 7230 §6.1)
 *        - copy inbound body (length-aware)
 *        - set timeout from pool->request_timeout_sec
 *        - add X-Forwarded-{For, Proto, Host}, Via
 *        - rewrite Host: to upstream when preserve_host=0
 *   6. Send. Classify error (transport / timeout / 5xx / 4xx).
 *   7. Copy upstream status + headers (minus Hop-by-Hop) + body
 *      onto `res`. Add X-Cache: MISS when cache is bound.
 *   8. Optionally store in cache.
 *   9. Decrement upstream inflight; record breaker outcome.
 *   10. Return 0 (short-circuit).
 *
 * Refuses requests with `Upgrade:` headers (WebSocket / h2 upstream)
 * with 502 + X-Aether-Proxy-Error: upgrade_unsupported. v2 follow-up.
 */

#include "aether_proxy_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#if defined(_WIN32)
#  include <windows.h>
#endif

/* The std.http.client surface. Included directly now that the client and
 * server headers can coexist (#1433): this file used to hand-declare these
 * prototypes, which meant the compiler could not check them against the real
 * definitions. */
#include "../../net/aether_http.h"
#include "../../net/aether_http_internal.h"

/* ----- hop-by-hop headers (RFC 7230 §6.1) ----- */

/* The always-hop-by-hop headers, with their lengths and lowercased first
 * letters alongside. Both are what lets the test below reject a header
 * without comparing strings, and computing them here means not computing
 * them on every header of every request. */
typedef struct {
    const char* name;
    size_t      len;
    char        first;
} HopHeader;

static const HopHeader HOP_HEADERS[] = {
    { "Connection",          10, 'c' },
    { "Keep-Alive",          10, 'k' },
    { "Proxy-Authenticate",  18, 'p' },
    { "Proxy-Authorization", 19, 'p' },
    { "TE",                   2, 't' },
    { "Trailer",              7, 't' },
    { "Transfer-Encoding",   17, 't' },
    { "Upgrade",              7, 'u' },
    { "Proxy-Connection",    16, 'p' },   /* legacy */
    { NULL,                   0, 0   }
};

/* Is this one of the always-hop-by-hop headers?
 *
 * Called for every header of every proxied request, and it used to run
 * strcasecmp against all nine names each time: about ninety case-insensitive
 * comparisons per request, which put __strcasecmp at the top of this path's
 * userspace profile.
 *
 * The list is fixed and known, so almost every header can be rejected without
 * comparing anything: no two entries share a length and a first letter except
 * the three beginning with "Tr"/"TE" and the three with "Proxy", and a length
 * test separates those. What reaches strcasecmp is a header that really might
 * be one of them.
 */
static int is_hop_by_hop(const char* name) {
    if (!name || !*name) return 0;
    size_t len = strlen(name);
    char first = (char)tolower((unsigned char)name[0]);

    for (const HopHeader* p = HOP_HEADERS; p->name; p++) {
        /* Length and first letter are a load and two compares, where
         * strcasecmp walks both strings lowercasing as it goes. */
        if (p->len != len || p->first != first) continue;
        if (strcasecmp(name, p->name) == 0) return 1;
    }
    return 0;
}

static int token_list_contains(const char* csv, const char* needle) {
    if (!csv || !*csv) return 1;
    if (!needle || !*needle) return 0;
    const char* p = csv;
    size_t nl = strlen(needle);
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char* start = p;
        while (*p && *p != ',') p++;
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - start) == nl && strncasecmp(start, needle, nl) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Is this header hop-by-hop for THIS message?
 *
 * The fixed list above is the always-hop-by-hop set. The rest is per-message:
 * a sender names its own connection-local headers in `Connection`, and RFC
 * 9110 7.6.1 requires an intermediary to drop every one of them.
 *
 * Passing them on is not merely untidy. A sender marks a header
 * connection-local precisely so the next hop does not see it, so a backend
 * that trusts a header (an internal auth header, a client-address header)
 * stays reachable through an intermediary that ignores the instruction.
 */
static int is_hop_by_hop_for(const char* name, const char* connection_value) {
    if (is_hop_by_hop(name)) return 1;
    if (!connection_value || !*connection_value) return 0;
    return token_list_contains(connection_value, name);
}

static int route_pattern_matches(const char* pattern, const char* path) {
    if (!pattern || !*pattern) return 1;
    if (!path) path = "/";
    if (strcmp(pattern, path) == 0) return 1;

    const char* p = pattern;
    const char* u = path;
    while (*p && *u) {
        if (*p == ':') {
            p++;
            while (*p && *p != '/') p++;
            while (*u && *u != '/') u++;
        } else if (*p == '*') {
            return 1;
        } else if (*p == *u) {
            p++;
            u++;
        } else {
            return 0;
        }
    }
    return *p == '\0' && *u == '\0';
}

/* ----- helpers ----- */

static const char* client_ip_for_xff(HttpRequest* req) {
    const char* xri = http_get_header(req, "X-Real-IP");
    if (xri && *xri) return xri;
    /* Best-effort fallback. The HTTP server doesn't currently
     * expose the connection's remote-address back to middleware
     * at a stable address, so we use "unknown" rather than
     * lying. Operators who care should register
     * middleware.use_real_ip first; that populates X-Real-IP. */
    return "unknown";
}

/* Append `value` to an existing comma-separated header value.
 * Returns a malloc'd string the caller frees. */
static char* append_csv(const char* existing, const char* value) {
    if (!existing || !*existing) return strdup(value ? value : "");
    size_t a = strlen(existing);
    size_t b = strlen(value);
    char* out = (char*)malloc(a + 2 + b + 1);
    if (!out) return NULL;
    memcpy(out, existing, a);
    out[a] = ',';
    out[a + 1] = ' ';
    memcpy(out + a + 2, value, b);
    out[a + 2 + b] = '\0';
    return out;
}

/* Build "<base_url><path>" with single slash boundary. Caller frees. */
static char* build_upstream_url(const char* base_url,
                                const char* path,
                                const char* query) {
    if (!base_url) return NULL;
    if (!path) path = "/";
    size_t bl = strlen(base_url);
    int chop = (bl > 0 && base_url[bl - 1] == '/' && path[0] == '/');
    size_t pl = strlen(path);
    size_t ql = (query && *query) ? strlen(query) + 1 : 0;  /* +1 for '?' */
    size_t out_len = (chop ? bl - 1 : bl) + pl + ql + 1;
    char* out = (char*)malloc(out_len);
    if (!out) return NULL;
    size_t pos = 0;
    if (chop) {
        memcpy(out + pos, base_url, bl - 1); pos += bl - 1;
    } else {
        memcpy(out + pos, base_url, bl); pos += bl;
    }
    memcpy(out + pos, path, pl); pos += pl;
    if (query && *query) {
        out[pos++] = '?';
        memcpy(out + pos, query, strlen(query)); pos += strlen(query);
    }
    out[pos] = '\0';
    return out;
}

/* Extract the host:port piece of a URL ("http://host:port/...") for
 * Host: rewriting. Returns malloc'd string, caller frees. NULL on
 * malformed.
 *
 * Called once per upstream when the pool registers it, not once per
 * forwarded request: base_url cannot change while the upstream exists, and
 * this was an allocation and a free on every request to recompute the same
 * bytes (#1739). */
char* aether_proxy_authority_of(const char* url) {
    if (!url) return NULL;
    const char* p = strstr(url, "://");
    if (!p) return NULL;
    p += 3;
    const char* end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    size_t n = (size_t)(end - p);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/* ----- response writer ----- */

/* Apply a cache hit's snapshot to the inbound HttpServerResponse. */
static void serve_from_cache(HttpServerResponse* res,
                             AetherProxyCacheEntry* e) {
    http_response_set_status(res, e->status_code);
    /* Replay headers (minus hop-by-hop, just in case). Also skip
     * X-Cache: cache_store snapshots the outbound response, which
     * carries our own X-Cache: MISS marker from the original miss.
     * Replaying that and then setting HIT below would either
     * duplicate the header or stick MISS depending on whether the
     * underlying setter appends or replaces. Owning the slot
     * exclusively is the unambiguous fix. */
    for (int i = 0; i < e->header_count; i++) {
        if (is_hop_by_hop(e->header_keys[i])) continue;
        if (strcasecmp(e->header_keys[i], "X-Cache") == 0) continue;
        http_response_set_header(res, e->header_keys[i], e->header_values[i]);
    }
    http_response_set_header(res, "X-Cache", "HIT");
    if (e->body && e->body_length > 0) {
        http_response_set_body_n(res, e->body, e->body_length);
    } else {
        http_response_set_body(res, "");
    }
}

/* ----- HTTP method classification + retry helpers ----- */

/* Idempotent per HTTP semantics (RFC 7231 §4.2.2). POST/PATCH
 * are NOT idempotent — silent retry could create duplicate
 * resources or apply patches twice, so we never retry them. */
static int method_is_idempotent(const char* method) {
    if (!method) return 0;
    return (strcmp(method, "GET")     == 0 ||
            strcmp(method, "HEAD")    == 0 ||
            strcmp(method, "PUT")     == 0 ||
            strcmp(method, "DELETE")  == 0 ||
            strcmp(method, "OPTIONS") == 0);
}

/* Sleep `ms` milliseconds. Used for retry backoff. */
static void retry_sleep_ms(int ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Compute next retry delay with exponential backoff + full jitter.
 * `attempt` is 1-indexed (first retry attempt = 1). Cap at 10s
 * so we don't pause for minutes on a deep backoff sequence. */
static int retry_backoff_ms(int base_ms, int attempt) {
    if (base_ms <= 0) return 0;
    /* Exponential: base * 2^(attempt-1), capped at 10000ms. */
    long max_ms = (long)base_ms;
    for (int i = 1; i < attempt && max_ms < 10000; i++) max_ms *= 2;
    if (max_ms > 10000) max_ms = 10000;
    /* Full jitter: random in [0, max_ms]. rand() is fine here —
     * jitter just spreads the thundering herd. */
    int jitter = (int)((long)rand() * max_ms / RAND_MAX);
    if (jitter < 0) jitter = 0;
    return jitter;
}

/* W3C Trace-Context: generate a 32-hex-digit trace-id and
 * 16-hex-digit span-id. rand() is sufficient for trace IDs —
 * they're not security-sensitive. */
static void trace_gen_id(char* out, size_t hex_chars) {
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < hex_chars; i++) {
        out[i] = hex[rand() & 0xf];
    }
    out[hex_chars] = '\0';
}

/* Inject `traceparent` if absent. The W3C format (version 00):
 *   00-<32hex trace-id>-<16hex span-id>-<flags>
 * flags=01 indicates "sampled". */
static void inject_traceparent_if_absent(HttpRequest* inbound,
                                         HttpClientRequest* outbound) {
    const char* existing = http_get_header(inbound, "traceparent");
    if (existing && *existing) {
        /* Pass through verbatim — operators may also `set_trace_inject(0)`
         * to skip auto-injection entirely; this branch fires regardless,
         * preserving end-to-end client-driven tracing. */
        http_request_set_header_raw(outbound, "traceparent", existing);
        const char* state = http_get_header(inbound, "tracestate");
        if (state && *state) {
            http_request_set_header_raw(outbound, "tracestate", state);
        }
        return;
    }
    /* Fresh trace. */
    char trace_id[33], span_id[17];
    trace_gen_id(trace_id, 32);
    trace_gen_id(span_id,  16);
    char header[80];
    snprintf(header, sizeof(header), "00-%s-%s-01", trace_id, span_id);
    http_request_set_header_raw(outbound, "traceparent", header);
}

/* ----- the middleware function itself ----- */

/* ---- The proxied request as a resumable exchange ----
 *
 * One upstream call is the only place this path waits on I/O, so it is the
 * only place that has to be suspendable. Splitting the request into the work
 * before the send, the send itself, and the work after it lets a caller that
 * cannot block drive the same code by supplying the send, without a second
 * implementation of the proxy semantics to keep in step with this one.
 *
 * The step functions return the middleware's own result code, 0 or 1, when
 * the exchange is finished, and PX_NEED_SEND when `outbound` is built and
 * waiting to go. Retries stay inside: a caller only ever sends and resumes.
 */

static int px_build(AetherProxyExchange* px);
static int px_classify(AetherProxyExchange* px, long elapsed_ms);
static int px_copy(AetherProxyExchange* px);

int aether_proxy_exchange_begin(AetherProxyExchange* px,
                                HttpRequest* req,
                                HttpServerResponse* res,
                                void* user_data,
                                struct HttpArena* arena) {
    memset(px, 0, sizeof(*px));
    /* Set before anything builds an outbound request, because begin() builds
     * the first one itself. NULL is the ordinary allocator. */
    px->arena = arena;
    AetherProxyOpts* opts = (AetherProxyOpts*)user_data;
    if (!opts || !opts->pool) return 1;  /* misconfigured — pass through */
    if (!req || !res) return 1;

    if (!token_list_contains(opts->methods, req->method)) {
        return 1;
    }

    if (opts->route_pattern &&
        !route_pattern_matches(opts->route_pattern, req->path)) {
        return 1;
    }

    /* Path-prefix gate. Empty prefix or "/" matches everything. */
    const char* prefix = opts->path_prefix ? opts->path_prefix : "/";
    if (!opts->route_pattern && *prefix && strcmp(prefix, "/") != 0) {
        size_t pl = strlen(prefix);
        if (!req->path || strncmp(req->path, prefix, pl) != 0) {
            return 1;
        }
    }

    /* Refuse Upgrade-bearing requests — WebSocket / h2 upstream
     * support is v2 follow-up. The client expects an upgrade and
     * we'd half-perform it, so be explicit. */
    const char* upgrade = http_get_header(req, "Upgrade");
    if (upgrade && *upgrade) {
        http_response_set_status(res, 502);
        http_response_set_header(res, "X-Aether-Proxy-Error", "upgrade_unsupported");
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res,
            "Upgrade headers are not forwarded by std.http.proxy v1.\n");
        return 0;
    }

    /* Body cap check. */
    if (req->body_length > (size_t)opts->max_body_bytes) {
        http_response_set_status(res, 502);
        http_response_set_header(res, "X-Aether-Proxy-Error", "request_body_too_large");
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res, "request body exceeds proxy cap\n");
        return 0;
    }

    /* Strip the configured prefix from the path before forwarding.
     * "/api/users" with strip "/api" becomes "/users". When
     * path_prefix matches but strip is unset, the full path is
     * forwarded as-is. */
    const char* forward_path = req->path ? req->path : "/";
    if (opts->strip_path_prefix && *opts->strip_path_prefix) {
        size_t sl = strlen(opts->strip_path_prefix);
        if (strncmp(forward_path, opts->strip_path_prefix, sl) == 0) {
            forward_path = forward_path + sl;
            if (!*forward_path) forward_path = "/";
        }
    }
    /* ---- Cache lookup ---- */
    if (opts->cache &&
        req->method &&
        (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0)) {
        AetherProxyCacheEntry* hit = aether_proxy_cache_lookup(
            opts->cache, req->method, forward_path, req);
        if (hit) {
            atomic_fetch_add(&opts->pool->metric_cache_hits, 1);
            serve_from_cache(res, hit);
            return 0;
        }
        atomic_fetch_add(&opts->pool->metric_cache_misses, 1);
    }
    /* ---- Pick upstream ---- */
    AetherUpstream* u = aether_proxy_lb_pick(opts->pool, req);
    if (!u) {
        atomic_fetch_add(&opts->pool->metric_503_no_upstream, 1);
        http_response_set_status(res, 503);
        http_response_set_header(res, "X-Aether-Proxy-Error", "no_upstream");
        http_response_set_header(res, "Retry-After", "1");
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res,
            "no upstream available (every upstream is unhealthy or "
            "the breaker is open)\n");
        return 0;
    }
    /* ---- Send (retry-aware) ----
     *
     * Build a fresh outbound request per attempt because the http
     * client consumes its request on send. Idempotent methods
     * (GET/HEAD/PUT/DELETE/OPTIONS) get retried on transport
     * failure or 5xx upstream when opts->retry_max_retries > 0.
     * Non-idempotent methods (POST/PATCH) are sent once.
     *
     * Re-pick semantics (matches nginx `proxy_next_upstream`):
     * each retry calls aether_proxy_lb_pick again, so retries
     * naturally route around a single failing upstream. The
     * picker's eligibility filters (healthy + breaker + drain +
     * rate-limit) keep us from re-hitting a known-bad target.
     * If no other upstream is eligible the picker may return
     * the same one — that's fine, we still get the backoff.
     *
     * Per-attempt metrics:
     *   - latency_sum_ms / latency_count: every completed attempt
     *   - transport_errors / timeouts: failed-without-status attempts
     *   - retries: every attempt past the first
     *   - requests_{2xx,3xx,4xx,5xx}: classified by final status
     *
     * Breaker outcome: only the FINAL attempt's outcome is recorded
     * against the upstream that produced it, because retries that
     * recover the call shouldn't count as failures against the
     * breaker (otherwise retry behaviour trips the breaker faster
     * than it should). The intermediate failed upstreams DO get
     * their own breaker_record(0) when we re-pick away from them. */
    px->opts = opts;
    px->req = req;
    px->res = res;
    px->u = u;
    px->forward_path = forward_path;
    px->attempt = 0;
    px->max_attempts = method_is_idempotent(req->method)
                     ? (1 + opts->retry_max_retries) : 1;
    return px_build(px);
}

static int px_build(AetherProxyExchange* px) {
    AetherProxyOpts* opts = px->opts;
    HttpRequest* req = px->req;
    HttpServerResponse* res = px->res;
    if (px->attempt > 0) {
        /* Backoff between attempts. */
        int sleep_ms = retry_backoff_ms(opts->retry_backoff_base_ms, px->attempt);
        retry_sleep_ms(sleep_ms);
        atomic_fetch_add(&px->u->metric_retries, 1);
    }

    /* ---- Build a fresh outbound request. ---- */
    px->upstream_url = build_upstream_url(px->u->base_url, px->forward_path,
                                          req->query_string);
    px->outbound = px->upstream_url ?
        http_request_raw_arena(req->method ? req->method : "GET",
                               px->upstream_url, px->arena) : NULL;
    if (!px->outbound) {
        free(px->upstream_url);
        px->upstream_url = NULL;
        aether_proxy_breaker_record(opts->pool, px->u, 0);
        aether_proxy_inflight_dec(px->u);
        http_response_set_status(res, 502);
        http_response_set_header(res, "X-Aether-Proxy-Error", "request_alloc_failed");
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res, "proxy request allocation failed\n");
        return 0;
    }
    if (opts->pool->request_timeout_sec > 0) {
        http_request_set_timeout_raw(px->outbound, opts->pool->request_timeout_sec);
    }

    /* Forward inbound headers minus hop-by-hop, including the ones this
     * particular request named in its own Connection header. */
    const char* inbound_connection = http_get_header(req, "Connection");
    for (int i = 0; i < req->header_count; i++) {
        const char* k = req->header_keys[i];
        const char* v = req->header_values[i];
        if (!k || is_hop_by_hop_for(k, inbound_connection)) continue;
        if (strcasecmp(k, "Host") == 0) continue;
        /* Skip traceparent/tracestate here — the Trace-Context
         * branch below handles them so the inject_traceparent
         * generation path can run when absent. */
        if (strcasecmp(k, "traceparent") == 0) continue;
        if (strcasecmp(k, "tracestate")  == 0) continue;
        http_request_set_header_raw(px->outbound, k, v ? v : "");
    }

    /* Host: rewrite. */
    if (opts->preserve_host) {
        const char* h = http_get_header(req, "Host");
        if (h && *h) http_request_set_header_raw(px->outbound, "Host", h);
    } else if (px->u->authority) {
        http_request_set_header_raw(px->outbound, "Host", px->u->authority);
    }

    /* X-Forwarded-* injection. */
    if (opts->add_xff) {
        const char* prior = http_get_header(req, "X-Forwarded-For");
        const char* client = client_ip_for_xff(req);
        char* xff = append_csv(prior, client);
        if (xff) {
            http_request_set_header_raw(px->outbound, "X-Forwarded-For", xff);
            free(xff);
        }
    }
    if (opts->add_xfp) {
        http_request_set_header_raw(px->outbound, "X-Forwarded-Proto", "http");
    }
    if (opts->add_xfh) {
        const char* h = http_get_header(req, "Host");
        if (h && *h) http_request_set_header_raw(px->outbound, "X-Forwarded-Host", h);
    }
    {
        const char* prior_via = http_get_header(req, "Via");
        char* via = append_csv(prior_via, "1.1 aether-proxy");
        if (via) {
            http_request_set_header_raw(px->outbound, "Via", via);
            free(via);
        }
    }

    /* W3C Trace-Context: pass inbound through verbatim if present;
     * otherwise generate a fresh trace when opts->trace_context_inject. */
    const char* tp = http_get_header(req, "traceparent");
    if (tp && *tp) {
        http_request_set_header_raw(px->outbound, "traceparent", tp);
        const char* ts = http_get_header(req, "tracestate");
        if (ts && *ts) http_request_set_header_raw(px->outbound, "tracestate", ts);
    } else if (opts->trace_context_inject) {
        inject_traceparent_if_absent(req, px->outbound);
    }

    /* Forward request body. */
    if (req->body && req->body_length > 0) {
        const char* ct = http_get_header(req, "Content-Type");
        http_request_set_body_raw(px->outbound, req->body,
                                  (int)req->body_length,
                                  ct ? ct : "application/octet-stream");
    }
    return PX_NEED_SEND;
}

int aether_proxy_exchange_resume(AetherProxyExchange* px, long elapsed_ms) {
    return px_classify(px, elapsed_ms);
}

static int px_classify(AetherProxyExchange* px, long elapsed_ms) {
    AetherProxyOpts* opts = px->opts;
    HttpRequest* req = px->req;
    HttpServerResponse* res = px->res;
    if (px->resp) {
        atomic_fetch_add(&px->u->metric_latency_sum_ms, elapsed_ms);
        atomic_fetch_add(&px->u->metric_latency_count, 1);
    }

    if (!px->resp) {
        /* Allocation-class failure (rare). Record + bail; never
         * retried because OOM tends not to clear up in 100ms. */
        atomic_fetch_add(&px->u->metric_transport_errors, 1);
        aether_proxy_breaker_record(opts->pool, px->u, 0);
        aether_proxy_inflight_dec(px->u);
        http_response_set_status(res, 502);
        http_response_set_header(res, "X-Aether-Proxy-Error", "send_alloc_failed");
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res, "proxy upstream call alloc failed\n");
        return 0;
    }

    const char* err = http_response_error(px->resp);
    int status = http_response_status(px->resp);

    if (err && *err) {
        int is_timeout = strstr(err, "timeout") != NULL ||
                         strstr(err, "timed out") != NULL;
        if (is_timeout) atomic_fetch_add(&px->u->metric_timeouts, 1);
        else            atomic_fetch_add(&px->u->metric_transport_errors, 1);

        /* Retry transport failures on idempotent methods. */
        int can_retry = (px->attempt + 1 < px->max_attempts);
        if (can_retry) {
            /* This upstream failed: charge it against the breaker
             * (it's a real failure that happened, not just a retry-
             * absorbed transient), release inflight, and re-pick
             * for the next px->attempt. */
            aether_proxy_breaker_record(opts->pool, px->u, 0);
            aether_proxy_inflight_dec(px->u);
            http_response_free(px->resp);
            AetherUpstream* next_u = aether_proxy_lb_pick(opts->pool, req);
            if (!next_u) {
                /* Nothing else eligible — return 503. */
                atomic_fetch_add(&opts->pool->metric_503_no_upstream, 1);
                http_response_set_status(res, 503);
                http_response_set_header(res, "X-Aether-Proxy-Error", "no_upstream_after_retry");
                http_response_set_header(res, "Retry-After", "1");
                http_response_set_header(res, "Content-Type", "text/plain");
                http_response_set_body(res, "no upstream available after retries\n");
                return 0;
            }
            px->u = next_u;
            px->attempt++;
            return px_build(px);
        }
        /* Final px->attempt — surface the error. */
        int is_oversize_resp = strstr(err, "exceeds") != NULL;
        aether_proxy_breaker_record(opts->pool, px->u, 0);
        aether_proxy_inflight_dec(px->u);
        http_response_set_status(res, is_timeout ? 504 : 502);
        http_response_set_header(res, "X-Aether-Proxy-Error",
            is_timeout ? "upstream_timeout" :
            is_oversize_resp ? "response_too_large" : "upstream_transport");
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res, err);
        http_response_free(px->resp);
        return 0;
    }

    /* Status arrived. Tally by class. */
    if      (status >= 200 && status < 300) atomic_fetch_add(&px->u->metric_requests_2xx, 1);
    else if (status >= 300 && status < 400) atomic_fetch_add(&px->u->metric_requests_3xx, 1);
    else if (status >= 400 && status < 500) atomic_fetch_add(&px->u->metric_requests_4xx, 1);
    else if (status >= 500)                  atomic_fetch_add(&px->u->metric_requests_5xx, 1);

    /* Retry on 5xx if there's budget (idempotent guarantee
     * already enforced by px->max_attempts). */
    if (status >= 500 && (px->attempt + 1) < px->max_attempts) {
        /* Charge the failure to this upstream's breaker, release
         * inflight, and re-pick. */
        aether_proxy_breaker_record(opts->pool, px->u, 0);
        aether_proxy_inflight_dec(px->u);
        http_response_free(px->resp);
        AetherUpstream* next_u = aether_proxy_lb_pick(opts->pool, req);
        if (!next_u) {
            atomic_fetch_add(&opts->pool->metric_503_no_upstream, 1);
            http_response_set_status(res, 503);
            http_response_set_header(res, "X-Aether-Proxy-Error", "no_upstream_after_retry");
            http_response_set_header(res, "Retry-After", "1");
            http_response_set_header(res, "Content-Type", "text/plain");
            http_response_set_body(res, "no upstream available after retries\n");
            return 0;
        }
        px->u = next_u;
        px->attempt++;
        return px_build(px);
    }

    px->status = status;
    return px_copy(px);
}

static int px_copy(AetherProxyExchange* px) {
    AetherProxyOpts* opts = px->opts;
    HttpRequest* req = px->req;
    HttpServerResponse* res = px->res;
    /* A status reached us, so this attempt is the one that counts. Record
     * the breaker outcome: ok is 2xx/3xx/4xx, not-ok is 5xx, because a 4xx
     * is the client's error and not the upstream's fault. */
    int classified_ok = (px->status >= 200 && px->status < 500);
    aether_proxy_breaker_record(opts->pool, px->u, classified_ok);
    aether_proxy_inflight_dec(px->u);

    /* ---- Copy upstream response onto `res` ---- */
    http_response_set_status(res, px->status);

    /* Headers — parse the raw block, drop hop-by-hop. */
    const char* headers_block = http_response_headers(px->resp);
    int seen_content_type = 0;
    if (headers_block) {
        const char* p = headers_block;
        while (*p) {
            /* strchr for the carriage return rather than strstr for the pair:
             * the setup a two-byte needle costs is paid once per header, and
             * this loop runs for every header of every proxied response. */
            const char* cr = strchr(p, '\r');
            if (!cr || cr[1] != '\n') break;
            const char* eol = cr;
            const char* colon = memchr(p, ':', (size_t)(eol - p));
            if (colon) {
                size_t kl = (size_t)(colon - p);
                char keybuf[128];
                if (kl < sizeof(keybuf)) {
                    memcpy(keybuf, p, kl);
                    keybuf[kl] = '\0';
                    if (!is_hop_by_hop(keybuf)) {
                        const char* v = colon + 1;
                        while (v < eol && (*v == ' ' || *v == '\t')) v++;
                        size_t vl = (size_t)(eol - v);

                        /* The value only has to be NUL-terminated for the
                         * setter. Doing that with an allocation cost a
                         * malloc and a free for every header of every
                         * response; a header value longer than this is rare
                         * enough to be worth one. */
                        char  stackval[512];
                        char* val = stackval;
                        if (vl >= sizeof(stackval)) {
                            val = (char*)malloc(vl + 1);
                            if (!val) { p = eol + 2; continue; }
                        }
                        memcpy(val, v, vl);
                        val[vl] = '\0';
                        http_response_set_header(res, keybuf, val);
                        if (strcasecmp(keybuf, "Content-Type") == 0) seen_content_type = 1;
                        if (val != stackval) free(val);
                    }
                }
            }
            p = eol + 2;
        }
    }
    if (!seen_content_type) {
        http_response_set_header(res, "Content-Type", "application/octet-stream");
    }
    if (opts->cache) {
        http_response_set_header(res, "X-Cache", "MISS");
    }

    /* Body — length-aware (binary-safe). `http_response_body_length`
     * exposes the AetherString's stored byte count, so payloads with
     * embedded NULs (gzip, protobuf, images, length-prefixed binary
     * formats) round-trip verbatim. Use the BORROWED `_str` accessor: it
     * returns a plain `const char*` into the response that we immediately
     * copy via http_response_set_body_n below, before http_response_free
     * (unlike http_response_body, which now hands back a retained
     * AetherString for Aether callers). */
    const char* body = http_response_body_str(px->resp);
    int body_length = http_response_body_length(px->resp);
    if (body_length > opts->max_body_bytes) {
        http_response_set_status(res, 502);
        http_response_set_header(res, "X-Aether-Proxy-Error", "response_too_large");
        http_response_set_body(res, "px->resp response exceeds proxy cap\n");
        http_response_free(px->resp);
        return 0;
    }
    if (body && body_length > 0) {
        http_response_set_body_n(res, body, body_length);
    } else {
        http_response_set_body(res, "");
    }

    /* ---- Cache store ---- */
    if (opts->cache && headers_block && req->method &&
        (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0)) {
        aether_proxy_cache_store(opts->cache, req->method, px->forward_path, req,
                                 px->status, headers_block,
                                 body, body_length);
    }

    http_response_free(px->resp);
    return 0;
}

/* ---- answering without copying the response (#1758) --------------------- */

/* Append into a caller-owned buffer, growing it. Returns 0, or -1 when the
 * allocator refuses. */
static int dh_put(char** buf, size_t* cap, size_t* len, const char* s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t want = *cap ? *cap : 1024;
        while (want < *len + n + 1) want *= 2;
        char* grown = (char*)realloc(*buf, want);
        if (!grown) return -1;
        *buf = grown;
        *cap = want;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

/* The status a response line carries, or 0 when it does not carry one. Same
 * shape as the copying path's check: exactly three digits after the version. */
static int direct_status_of(const char* raw, size_t raw_len) {
    if (raw_len < 12 || strncmp(raw, "HTTP/", 5) != 0) return 0;
    const char* sp = memchr(raw, ' ', raw_len);
    if (!sp) return 0;
    const char* d = sp + 1;
    if ((size_t)(d - raw) + 3 > raw_len) return 0;
    if (!isdigit((unsigned char)d[0]) || !isdigit((unsigned char)d[1]) ||
        !isdigit((unsigned char)d[2])) return 0;
    if ((size_t)(d - raw) + 3 < raw_len && isdigit((unsigned char)d[3])) return 0;
    return (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
}

int aether_proxy_direct_ok(const AetherProxyExchange* px) {
    if (!px || !px->opts) return 0;
    /* A cache has to keep the headers and body, and a transformer has to be
     * able to change them. Both need the object this path does not build. */
    if (px->opts->cache) return 0;
    if (px->opts->direct_disabled) return 0;
    return 1;
}

int aether_proxy_direct_take(AetherProxyExchange* px, long elapsed_ms,
                             const char* raw, size_t raw_len,
                             size_t header_bytes, AetherProxyDirect* out) {
    if (!px || !px->opts || !raw || !out) return -1;
    if (header_bytes < 4 || header_bytes > raw_len) return -1;

    int status = direct_status_of(raw, header_bytes);
    if (status == 0) return -1;      /* no status line: not ours to pass on */

    /* Everything that can decline is decided before anything is recorded.
     * A decline sends the caller down the copying path, which does its own
     * recording, and a metric counted here as well would be counted twice. */

    /* A 5xx with retry budget left is not an answer, it is a failover the
     * copying path is about to perform against another upstream. Passing it
     * straight back to the client would turn a retried request into a served
     * error. */
    if (status >= 500 && (px->attempt + 1) < px->max_attempts) return -1;

    size_t body_len = raw_len - header_bytes;
    if ((long)body_len > (long)px->opts->max_body_bytes) return -1;

    /* The same bookkeeping the copying path does for a response that arrived,
     * in the same order, so the two cannot report differently. */
    atomic_fetch_add(&px->u->metric_latency_sum_ms, elapsed_ms);
    atomic_fetch_add(&px->u->metric_latency_count, 1);

    if      (status >= 200 && status < 300) atomic_fetch_add(&px->u->metric_requests_2xx, 1);
    else if (status >= 300 && status < 400) atomic_fetch_add(&px->u->metric_requests_3xx, 1);
    else if (status >= 400 && status < 500) atomic_fetch_add(&px->u->metric_requests_4xx, 1);
    else if (status >= 500)                 atomic_fetch_add(&px->u->metric_requests_5xx, 1);

    aether_proxy_breaker_record(px->opts->pool, px->u, status >= 200 && status < 500);
    aether_proxy_inflight_dec(px->u);

    px->status    = status;
    out->status   = status;
    out->body     = raw + header_bytes;
    out->body_len = body_len;
    return 0;
}

int aether_proxy_direct_head(const AetherProxyDirect* d,
                             const char* raw, size_t header_bytes,
                             char** buf, size_t* cap, size_t* out_len) {
    if (!d || !raw || !buf || !cap || !out_len) return -1;
    *out_len = 0;

    /* The status line carries our own reason phrase, not the upstream's,
     * because that is what the copying path emits: it sets the status by
     * number and the serializer looks the text up. */
    char line[96];
    const char* stxt = http_status_text(d->status);
    size_t stl = strlen(stxt);
    if (d->status < 0 || stl + 32 > sizeof(line)) return -1;
    size_t n = 9;
    memcpy(line, "HTTP/1.1 ", 9);
    n += http_write_dec(line + n, (unsigned long long)d->status);
    line[n++] = ' ';
    memcpy(line + n, stxt, stl);
    n += stl;
    line[n++] = '\r';
    line[n++] = '\n';
    if (dh_put(buf, cap, out_len, line, n) != 0) return -1;

    /* Content-Type and Server come first and always exist, because the
     * response object is created holding both and an upstream header of the
     * same name replaces the value in place rather than appending. Emitting
     * them here in that order is what keeps these bytes identical to the
     * copying path's. */
    const char* hstart = memchr(raw, '\n', header_bytes);
    if (!hstart) return -1;
    hstart += 1;
    const char* hend = raw + header_bytes - 2;   /* at the terminating CRLF */
    if (hend < hstart) return -1;

    const char* ct = NULL; size_t ct_len = 0;
    const char* sv = NULL; size_t sv_len = 0;

    for (const char* p = hstart; p < hend; ) {
        const char* cr = memchr(p, '\r', (size_t)(hend - p));
        if (!cr || cr + 1 >= raw + header_bytes || cr[1] != '\n') break;
        const char* colon = memchr(p, ':', (size_t)(cr - p));
        if (colon) {
            size_t kl = (size_t)(colon - p);
            const char* v = colon + 1;
            while (v < cr && (*v == ' ' || *v == '\t')) v++;
            if (kl == 12 && http_ci_eq(p, "Content-Type", 12)) {
                ct = v; ct_len = (size_t)(cr - v);
            } else if (kl == 6 && http_ci_eq(p, "Server", 6)) {
                sv = v; sv_len = (size_t)(cr - v);
            }
        }
        p = cr + 2;
    }

    if (dh_put(buf, cap, out_len, "Content-Type: ", 14) != 0) return -1;
    if (ct) { if (dh_put(buf, cap, out_len, ct, ct_len) != 0) return -1; }
    /* No upstream Content-Type: the copying path replaces the created
     * default with this one rather than leaving text/html. */
    else    { if (dh_put(buf, cap, out_len, "application/octet-stream", 24) != 0) return -1; }
    if (dh_put(buf, cap, out_len, "\r\n", 2) != 0) return -1;

    if (dh_put(buf, cap, out_len, "Server: ", 8) != 0) return -1;
    if (sv) { if (dh_put(buf, cap, out_len, sv, sv_len) != 0) return -1; }
    else    { if (dh_put(buf, cap, out_len, "Aether/1.0", 10) != 0) return -1; }
    if (dh_put(buf, cap, out_len, "\r\n", 2) != 0) return -1;

    /* Everything else the upstream sent, in arrival order, minus the two
     * already emitted and minus the hop-by-hop headers, which belong to the
     * connection this proxy terminated and not to the message. */
    int saw_content_length = 0;
    for (const char* p = hstart; p < hend; ) {
        const char* cr = memchr(p, '\r', (size_t)(hend - p));
        if (!cr || cr + 1 >= raw + header_bytes || cr[1] != '\n') break;
        const char* colon = memchr(p, ':', (size_t)(cr - p));
        if (!colon) { p = cr + 2; continue; }

        size_t kl = (size_t)(colon - p);
        if ((kl == 12 && http_ci_eq(p, "Content-Type", 12)) ||
            (kl == 6  && http_ci_eq(p, "Server", 6))) { p = cr + 2; continue; }

        char keybuf[128];
        if (kl >= sizeof(keybuf)) { p = cr + 2; continue; }
        memcpy(keybuf, p, kl);
        keybuf[kl] = '\0';
        if (is_hop_by_hop(keybuf)) { p = cr + 2; continue; }

        /* Content-Length describes the body being sent, which is this
         * proxy's to state and not the upstream's to be repeated: the
         * copying path sets it from the bytes it holds, so a HEAD response,
         * whose upstream length describes a body that is not being
         * forwarded, comes out as 0 rather than as that length. Emitted here
         * in the upstream's position so the header order matches. */
        if (kl == 14 && http_ci_eq(p, "Content-Length", 14)) {
            char cl[48];
            size_t cn = 16;
            memcpy(cl, "Content-Length: ", 16);
            cn += http_write_dec(cl + cn, (unsigned long long)d->body_len);
            cl[cn++] = '\r';
            cl[cn++] = '\n';
            if (dh_put(buf, cap, out_len, cl, cn) != 0) return -1;
            saw_content_length = 1;
            p = cr + 2;
            continue;
        }

        const char* v = colon + 1;
        while (v < cr && (*v == ' ' || *v == '\t')) v++;
        size_t vl = (size_t)(cr - v);

        /* A name or value carrying a line ending would end the head early and
         * let the rest be read as a second response (CWE-113). The copying
         * path drops such a header; so does this one. */
        if (!http_header_name_ok(keybuf) || memchr(v, '\r', vl) || memchr(v, '\n', vl)) {
            p = cr + 2; continue;
        }

        if (dh_put(buf, cap, out_len, keybuf, kl) != 0) return -1;
        if (dh_put(buf, cap, out_len, ": ", 2) != 0) return -1;
        if (vl && dh_put(buf, cap, out_len, v, vl) != 0) return -1;
        if (dh_put(buf, cap, out_len, "\r\n", 2) != 0) return -1;
        p = cr + 2;
    }

    /* An upstream that sent no Content-Length still gets one, appended, which
     * is where the copying path puts it when the header did not already
     * exist to be replaced. */
    if (!saw_content_length) {
        char cl[48];
        size_t cn = 16;
        memcpy(cl, "Content-Length: ", 16);
        cn += http_write_dec(cl + cn, (unsigned long long)d->body_len);
        cl[cn++] = '\r';
        cl[cn++] = '\n';
        if (dh_put(buf, cap, out_len, cl, cn) != 0) return -1;
    }

    return dh_put(buf, cap, out_len, "\r\n", 2);
}

/* The blocking driver: the server's worker owns the thread for the whole
 * request, so it simply performs the send itself. */
int aether_middleware_reverse_proxy(HttpRequest* req,
                                    HttpServerResponse* res,
                                    void* user_data) {
    AetherProxyExchange px;
    int r = aether_proxy_exchange_begin(&px, req, res, user_data, NULL);
    while (r == PX_NEED_SEND) {
        long t0 = aether_proxy_now_ms();
        px.resp = http_send_raw(px.outbound);
        long elapsed = aether_proxy_now_ms() - t0;
        http_request_free_raw(px.outbound);
        free(px.upstream_url);
        px.outbound = NULL;
        px.upstream_url = NULL;
        r = aether_proxy_exchange_resume(&px, elapsed);
    }
    return r;
}

/* ----- install ----- */

const char* aether_proxy_use_reverse_proxy(HttpServer* server,
                                           const char* path_prefix,
                                           AetherProxyPool* pool,
                                           AetherProxyOpts* opts) {
    return aether_proxy_use_reverse_proxy_methods(server, path_prefix, pool,
                                                  opts, NULL);
}

const char* aether_proxy_use_reverse_proxy_methods(HttpServer* server,
                                                   const char* path_prefix,
                                                   AetherProxyPool* pool,
                                                   AetherProxyOpts* opts,
                                                   const char* methods_csv) {
    if (!server) return "server is null";
    if (!pool)   return "pool is null";
    if (!opts)   return "opts is null";
    if (!path_prefix || *path_prefix != '/') return "path_prefix must start with /";

    /* Bind the pool to opts (refcount-incremented). The opts are
     * the user_data that the middleware receives. */
    aether_proxy_pool_retain(pool);
    if (opts->pool && opts->pool != pool) {
        /* Caller is rebinding. Release the prior pool first. */
        aether_proxy_pool_free(opts->pool);
    }
    opts->pool = pool;

    free(opts->path_prefix);
    free(opts->route_pattern);
    free(opts->methods);
    opts->route_pattern = NULL;
    opts->methods = (methods_csv && *methods_csv) ? strdup(methods_csv) : NULL;
    opts->path_prefix = strdup(path_prefix);
    if (!opts->path_prefix || ((methods_csv && *methods_csv) && !opts->methods)) {
        aether_proxy_pool_free(pool);  /* roll back retain */
        opts->pool = NULL;
        free(opts->path_prefix);
        opts->path_prefix = NULL;
        free(opts->methods);
        opts->methods = NULL;
        return "out of memory";
    }

    http_server_use_middleware(server, aether_middleware_reverse_proxy, opts);
    return "";
}

const char* aether_proxy_use_reverse_proxy_match(HttpServer* server,
                                                 const char* path_pattern,
                                                 AetherProxyPool* pool,
                                                 AetherProxyOpts* opts,
                                                 const char* methods_csv) {
    if (!server) return "server is null";
    if (!pool)   return "pool is null";
    if (!opts)   return "opts is null";
    if (!path_pattern || *path_pattern != '/') return "path_pattern must start with /";

    aether_proxy_pool_retain(pool);
    if (opts->pool && opts->pool != pool) {
        aether_proxy_pool_free(opts->pool);
    }
    opts->pool = pool;

    free(opts->path_prefix);
    free(opts->route_pattern);
    free(opts->methods);
    opts->path_prefix = strdup("/");
    opts->route_pattern = strdup(path_pattern);
    opts->methods = (methods_csv && *methods_csv) ? strdup(methods_csv) : NULL;
    if (!opts->path_prefix || !opts->route_pattern ||
        ((methods_csv && *methods_csv) && !opts->methods)) {
        aether_proxy_pool_free(pool);
        opts->pool = NULL;
        free(opts->path_prefix);
        free(opts->route_pattern);
        free(opts->methods);
        opts->path_prefix = NULL;
        opts->route_pattern = NULL;
        opts->methods = NULL;
        return "out of memory";
    }

    http_server_use_middleware(server, aether_middleware_reverse_proxy, opts);
    return "";
}

const char* aether_proxy_use_simple_proxy(HttpServer* server,
                                          const char* path_prefix,
                                          const char* upstream_url,
                                          int request_timeout_sec) {
    if (!server) return "server is null";
    if (!path_prefix || *path_prefix != '/') return "path_prefix must start with /";
    if (!upstream_url || !*upstream_url) return "upstream_url is empty";

    AetherProxyPool* pool = aether_proxy_pool_new(
        AETHER_PROXY_LB_ROUND_ROBIN,
        request_timeout_sec, 0, 0);
    if (!pool) return "pool allocation failed";

    const char* err = aether_proxy_upstream_add(pool, upstream_url, 1);
    if (err && *err) {
        aether_proxy_pool_free(pool);
        return err;
    }

    AetherProxyOpts* opts = aether_proxy_opts_new();
    if (!opts) {
        aether_proxy_pool_free(pool);
        return "opts allocation failed";
    }

    err = aether_proxy_use_reverse_proxy(server, path_prefix, pool, opts);
    /* The opts owns the pool refcount now via aether_proxy_use_reverse_proxy.
     * We drop our local one. */
    aether_proxy_pool_free(pool);
    return err;
}
