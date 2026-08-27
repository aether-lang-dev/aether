#include "aether_http.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_resource_caps.h"

#if !AETHER_HAS_NETWORKING
HttpResponse* http_get_raw(const char* u) { (void)u; return NULL; }
HttpResponse* http_get_with_timeout_ns_raw(const char* u, int64_t ns) { (void)u; (void)ns; return NULL; }
HttpResponse* http_post_raw(const char* u, const char* b, const char* c) { (void)u; (void)b; (void)c; return NULL; }
HttpResponse* http_put_raw(const char* u, const char* b, const char* c) { (void)u; (void)b; (void)c; return NULL; }
HttpResponse* http_delete_raw(const char* u) { (void)u; return NULL; }
void http_response_free(HttpResponse* r) { (void)r; }
int http_response_status(HttpResponse* r) { (void)r; return 0; }
const char* http_response_body(HttpResponse* r) { (void)r; return ""; }
int http_response_body_length(HttpResponse* r) { (void)r; return 0; }
const char* http_response_headers(HttpResponse* r) { (void)r; return ""; }
const char* http_response_error(HttpResponse* r) { (void)r; return "networking disabled at build time"; }
int http_response_ok(HttpResponse* r) { (void)r; return 0; }
struct HttpClientRequest { int unused; };
HttpClientRequest* http_request_raw(const char* m, const char* u) { (void)m; (void)u; return NULL; }
int http_request_set_header_raw(HttpClientRequest* r, const char* n, const char* v) { (void)r; (void)n; (void)v; return -1; }
int http_request_set_body_raw(HttpClientRequest* r, const char* b, int l, const char* c) { (void)r; (void)b; (void)l; (void)c; return -1; }
int http_request_set_timeout_raw(HttpClientRequest* r, int s) { (void)r; (void)s; return -1; }
int http_request_set_timeout_ns_raw(HttpClientRequest* r, int64_t ns) { (void)r; (void)ns; return -1; }
int http_request_set_follow_redirects_raw(HttpClientRequest* r, int n) { (void)r; (void)n; return -1; }
int http_request_set_insecure_raw(HttpClientRequest* r, int on) { (void)r; (void)on; return -1; }
int http_request_set_cafile_raw(HttpClientRequest* r, const char* p) { (void)r; (void)p; return -1; }
int http_request_use_env_proxy_raw(HttpClientRequest* r, int on) { (void)r; (void)on; return -1; }
int http_request_use_http_proxy_raw(HttpClientRequest* r, const char* u) { (void)r; (void)u; return -1; }
int http_request_ignore_http_proxy_raw(HttpClientRequest* r) { (void)r; return -1; }
void http_request_free_raw(HttpClientRequest* r) { (void)r; }
HttpResponse* http_send_raw(HttpClientRequest* r) { (void)r; return NULL; }
const char* http_response_header_raw(HttpResponse* r, const char* n) { (void)r; (void)n; return ""; }
const char* http_response_effective_url_raw(HttpResponse* r) { (void)r; return ""; }
const char* http_response_redirect_error_raw(HttpResponse* r) { (void)r; return ""; }
int http_request_set_stream_raw(HttpClientRequest* r, int on) { (void)r; (void)on; return -1; }
int http_response_is_stream_raw(HttpResponse* r) { (void)r; return 0; }
const char* http_response_read_chunk_raw(HttpResponse* r, int max) { (void)r; (void)max; return ""; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "../../runtime/utils/aether_thread.h"
#include <limits.h>
#include <stdint.h>   /* INT64_MAX, for the pool expiry watermark */

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <poll.h>        /* poll: no FD_SETSIZE bound, unlike select   */
    #include <fcntl.h>       /* fcntl, O_NONBLOCK for connect-with-timeout */
    #include <errno.h>       /* EINPROGRESS / EWOULDBLOCK detection      */
    #include <sys/select.h>  /* select(), fd_set                          */
    #include <sys/time.h>    /* struct timeval                            */
#endif

#ifdef AETHER_HAS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/x509v3.h>
#endif

static int http_initialized = 0;

static void http_init(void) {
    if (http_initialized) return;
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    http_initialized = 1;
}

// -----------------------------------------------------------------
// URL parsing
// -----------------------------------------------------------------

// Parse `url` into (host, port, path, use_tls).
//   https://foo.example/bar  →  host="foo.example" port=443 path="/bar" use_tls=1
//   http://foo:8080/         →  host="foo"         port=8080 path="/"   use_tls=0
// Returns 1 on success, 0 on malformed input.
static int parse_url(const char* url, char* host, size_t host_size,
                     int* port, char* path, size_t path_size, int* use_tls) {
    if (!url || !host || !port || !path || !use_tls ||
        host_size == 0 || path_size == 0) return 0;

    const char* start;
    *use_tls = 0;

    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
        *port = 80;
    } else if (strncmp(url, "https://", 8) == 0) {
        start = url + 8;
        *port = 443;
        *use_tls = 1;
    } else {
        start = url;
        *port = 80;
    }

    const char* slash = strchr(start, '/');
    const char* colon = strchr(start, ':');

    if (colon && (!slash || colon < slash)) {
        size_t host_len = colon - start;
        if (host_len >= host_size) host_len = host_size - 1;
        memcpy(host, start, host_len);
        host[host_len] = '\0';
        *port = atoi(colon + 1);
        if (slash) {
            snprintf(path, path_size, "%s", slash);
        } else {
            snprintf(path, path_size, "/");
        }
    } else if (slash) {
        size_t host_len = slash - start;
        if (host_len >= host_size) host_len = host_size - 1;
        memcpy(host, start, host_len);
        host[host_len] = '\0';
        snprintf(path, path_size, "%s", slash);
    } else {
        snprintf(host, host_size, "%s", start);
        snprintf(path, path_size, "/");
    }

    return 1;
}

// -----------------------------------------------------------------
// OpenSSL context: lazy init, shared across all HTTPS calls
// -----------------------------------------------------------------

#ifdef AETHER_HAS_OPENSSL

static _Atomic(SSL_CTX*) g_ssl_ctx;

#ifdef _WIN32
// Probe well-known CA-bundle locations on Windows in priority order and
// load the first one that exists. Linux + macOS have a system trust
// store that OpenSSL's compiled-in default paths point at; Windows
// MSYS2 builds keep theirs at MINGW_PREFIX/etc/ssl/certs/ca-bundle.crt
// (from the `mingw-w64-x86_64-ca-certificates` package), and the Aether
// release archive includes a bundle next to ae.exe at
// <root>/share/ssl/ca-bundle.crt. Without a trust anchor every HTTPS
// request fails with "certificate verify failed".
//
// Returns 1 if a bundle was successfully loaded, 0 if no probe matched.
static int load_windows_ca_bundle(SSL_CTX* ctx) {
    // 1. SSL_CERT_FILE env var — also honored by SSL_CTX_set_default_
    //    verify_paths, but try it first so the load succeeds even when
    //    the default paths weren't compiled in.
    const char* env = getenv("SSL_CERT_FILE");
    if (env && env[0] && SSL_CTX_load_verify_locations(ctx, env, NULL) == 1) {
        return 1;
    }

    // 2. Bundle shipped alongside ae.exe in the release archive
    //    (<root>/bin/ae.exe → <root>/share/ssl/ca-bundle.crt).
    char exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (n > 0 && n < sizeof(exe_path)) {
        char* p = strrchr(exe_path, '\\');           // …\bin\ae.exe → …\bin
        if (p) {
            *p = '\0';
            char* q = strrchr(exe_path, '\\');        // …\bin → …
            if (q && _stricmp(q, "\\bin") == 0) *q = '\0';
        }
        char bundle[MAX_PATH + 64];
        snprintf(bundle, sizeof(bundle),
                 "%s\\share\\ssl\\ca-bundle.crt", exe_path);
        if (SSL_CTX_load_verify_locations(ctx, bundle, NULL) == 1) {
            return 1;
        }
    }

    // 3. MSYS2 well-known paths (mingw-w64-x86_64-ca-certificates package).
    static const char* candidates[] = {
        "C:\\msys64\\mingw64\\etc\\ssl\\certs\\ca-bundle.crt",
        "C:\\msys64\\ucrt64\\etc\\ssl\\certs\\ca-bundle.crt",
        "C:\\msys64\\mingw64\\etc\\ssl\\cert.pem",
        "C:\\msys64\\ucrt64\\etc\\ssl\\cert.pem",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (SSL_CTX_load_verify_locations(ctx, candidates[i], NULL) == 1) {
            return 1;
        }
    }
    return 0;
}
#endif  // _WIN32

// Get (or lazily create) the shared SSL_CTX. Benign race on first call —
// compare-exchange ensures at most one SSL_CTX is installed even if two
// threads reach here simultaneously. Returns NULL on OpenSSL error.
static SSL_CTX* get_ssl_ctx(void) {
    SSL_CTX* ctx = atomic_load(&g_ssl_ctx);
    if (ctx) return ctx;

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

#ifdef _WIN32
    // Windows OpenSSL builds typically have no compiled-in CA path — probe
    // SSL_CERT_FILE / the shipped bundle / MSYS2 paths first; only fall
    // through to the default-paths call (which is effectively a no-op on
    // Win32) if every probe missed.
    if (!load_windows_ca_bundle(ctx)) {
        SSL_CTX_set_default_verify_paths(ctx);
    }
#else
    // Linux/macOS: the system trust store lives at the path OpenSSL was
    // compiled with — /etc/ssl/certs and the macOS keychain bridge.
    SSL_CTX_set_default_verify_paths(ctx);
#endif

    SSL_CTX* expected = NULL;
    if (!atomic_compare_exchange_strong(&g_ssl_ctx, &expected, ctx)) {
        SSL_CTX_free(ctx);
        ctx = expected;
    }
    return ctx;
}

// Flush OpenSSL's error queue into a newly allocated string. Used only
// on error paths to surface the underlying OpenSSL reason.
static char* ssl_err_string(const char* prefix) {
    unsigned long err = ERR_get_error();
    const char* detail = err ? ERR_reason_error_string(err) : NULL;
    size_t plen = strlen(prefix);
    size_t dlen = detail ? strlen(detail) : 0;
    char* msg = (char*)malloc(plen + (dlen ? dlen + 3 : 0) + 1);
    if (!msg) return NULL;
    memcpy(msg, prefix, plen);
    if (dlen) {
        memcpy(msg + plen, ": ", 2);
        memcpy(msg + plen + 2, detail, dlen);
        msg[plen + 2 + dlen] = '\0';
    } else {
        msg[plen] = '\0';
    }
    return msg;
}

#endif // AETHER_HAS_OPENSSL

// -----------------------------------------------------------------
// Transport abstraction
//
// `Transport` wraps either a raw socket or an SSL* connection so the
// request/response loop doesn't need to branch on protocol everywhere.
// send/recv callbacks match the BSD socket signatures so plaintext
// paths can use them as-is.
// -----------------------------------------------------------------

typedef struct {
    int sockfd;
    /* The SO_RCVTIMEO/SO_SNDTIMEO value currently on this socket, or -1 when
     * nothing has been applied yet (#1719).
     *
     * A Transport travels with its connection into the idle pool, so a reused
     * connection already carries the timeouts the last request set. Re-applying
     * an identical value costs 2 setsockopt syscalls per request and changes
     * nothing: under strace against the LB benchmark, setsockopt was the third
     * costliest syscall at 202,552 calls for 20,000 requests -- roughly 10 per
     * request, on sockets whose options were already correct.
     *
     * A sentinel of -1 rather than 0 because 0 is a legitimate timeout value
     * meaning "block indefinitely", and a socket set to block forever must not
     * be confused with one never configured. */
    int64_t applied_timeout_ns;
#ifdef AETHER_HAS_OPENSSL
    SSL* ssl;
    /* A per-request SSL_CTX, owned by this transport, or NULL when the
     * shared process-wide CTX was used. Non-NULL only for the set_cafile
     * pin path (#1107/#1110), which needs its own CTX whose trust store is
     * loaded from the couriered CA. Freed in transport_close AFTER the SSL
     * that references it. */
    SSL_CTX* owned_ctx;
#endif
} Transport;

static int transport_send(Transport* t, const void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) return SSL_write(t->ssl, buf, len);
#endif
    return (int)send(t->sockfd, buf, len, 0);
}

static int transport_recv(Transport* t, void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) return SSL_read(t->ssl, buf, len);
#endif
    return (int)recv(t->sockfd, buf, len, 0);
}

static void transport_close(Transport* t) {
#ifdef AETHER_HAS_OPENSSL
    if (t->ssl) {
        SSL_shutdown(t->ssl);
        SSL_free(t->ssl);
        t->ssl = NULL;
    }
    /* Free the per-request CTX (set_cafile pin) AFTER the SSL that used it. */
    if (t->owned_ctx) {
        SSL_CTX_free(t->owned_ctx);
        t->owned_ctx = NULL;
    }
#endif
    if (t->sockfd >= 0) {
        close(t->sockfd);
        t->sockfd = -1;
    }
}

// -----------------------------------------------------------------
// Streaming response bodies (#1004)
//
// A streaming response keeps its transport open after the header block
// and delivers the body incrementally, so a multi-megabyte download never
// materialises whole in memory: peak usage is one read window plus the
// small `pending` buffer, not O(Content-Length). The HttpStream carries
// the open transport, the body framing (Content-Length, chunked, or
// read-until-close), and `pending` — raw socket bytes already read but not
// yet decoded/delivered (the over-read past the header terminator, and
// chunk-framing bytes that straddle recv boundaries).
// -----------------------------------------------------------------

struct HttpStream {
    Transport t;
    int  chunked;             /* Transfer-Encoding: chunked */
    int  read_until_close;    /* no Content-Length and not chunked: body ends at EOF */
    long long content_remaining; /* Content-Length framing: undelivered body bytes */
    /* chunked decode state machine: 0=read size line, 1=deliver chunk data,
     * 2=consume the CRLF that trails a chunk's data. */
    int  chunk_state;
    long long chunk_remaining; /* chunked: undelivered payload bytes in current chunk */
    int  eof;                 /* body fully delivered (nothing more will arrive) */
    int  err;                 /* transport error mid-body */
    char* pending;            /* raw bytes read from transport, not yet consumed */
    int   pending_pos;
    int   pending_len;
    int   pending_cap;
};

// -----------------------------------------------------------------
// Idle connection pool (#1653)
//
// HTTP/1.1 connections are persistent, and a client that closes after every
// response pays a TCP handshake (and a TLS handshake) per request. Measured
// against nginx, that churn was the whole of std.http.server.lb's 3.7x deficit
// and its 8% dropped requests: the proxy dialled its upstream once per
// request.
//
// A connection is only returned here when the response framing was definite
// (Content-Length or chunked) and neither side asked to close, so nothing is
// ever reused when the connection itself was the message delimiter. Entries
// are keyed by everything that makes two connections non-interchangeable:
// origin, the proxy actually dialled, TLS, and the verification the caller
// asked for. A pooled socket the peer closed while idle is indistinguishable
// from a live one until it is used, so the request path retries once on a
// fresh connection when a reused one fails before any response byte arrives.
// -----------------------------------------------------------------

#define HTTP_POOL_KEY_MAX 512

typedef struct HttpIdleConn {
    Transport t;
    char      key[HTTP_POOL_KEY_MAX];
    int64_t   idle_since_ms;
    struct HttpIdleConn* next;
} HttpIdleConn;

/* CRITICAL_SECTION, which the thread shim maps pthread_mutex_t to on Windows,
 * has no static initialiser, so the lock is created once through an atomic
 * gate there. Same shape as runtime/sandbox/aether_audit.c. */
#if defined(_WIN32)
static pthread_mutex_t http_pool_mutex;
static atomic_int      http_pool_mutex_state = 0;   /* 0 unset, 1 setting, 2 ready */
static pthread_mutex_t* http_pool_lock(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&http_pool_mutex_state, &expected, 1)) {
        pthread_mutex_init(&http_pool_mutex, NULL);
        atomic_store(&http_pool_mutex_state, 2);
    } else {
        while (atomic_load(&http_pool_mutex_state) != 2) { /* one-shot spin */ }
    }
    return &http_pool_mutex;
}
#else
static pthread_mutex_t http_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* http_pool_lock(void) { return &http_pool_mutex; }
#endif

static HttpIdleConn*   http_pool_head = NULL;
static int             http_pool_count = 0;
static int             http_pool_enabled = 1;
static int             http_pool_max_idle = 64;
static int             http_pool_max_per_key = 8;
static int64_t         http_pool_idle_ms = 15000;
/* When the oldest pooled connection becomes eligible for expiry, or INT64_MAX
 * when the pool is empty (#1719).
 *
 * http_pool_take and http_pool_put both swept the whole idle list on every
 * request, under the global pool mutex. With the default 15s idle window and a
 * proxy reusing its upstreams continuously, that sweep frees nothing on the
 * overwhelming majority of calls -- it is a list walk per request to discover
 * there is nothing to do. Tracking the earliest deadline lets both callers skip
 * the walk outright until that time actually arrives.
 *
 * Kept deliberately coarse: it is a lower bound, not an exact answer. Entries
 * are only ever added with a fresh idle_since_ms, so the earliest deadline can
 * only move later when the list shrinks, and recomputing it during a sweep we
 * were doing anyway is free. A stale-early value costs one redundant sweep,
 * never a missed expiry. */
static int64_t         http_pool_next_expiry_ms = INT64_MAX;

static int64_t http_now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return (int64_t)time(NULL) * 1000;
}

/* Everything that makes two connections non-interchangeable: the origin, the
 * endpoint actually dialled (proxy or origin), TLS, and the verification the
 * caller asked for. A connection opened with a pinned CA or with verification
 * off must never be handed to a request that did not ask for that. */
static void http_pool_key(char* out, size_t n, const char* host, int port,
                          int use_tls, const char* dial_host, int dial_port,
                          int insecure, const char* cafile) {
    snprintf(out, n, "%s:%d|%s:%d|%d|%d|%s",
             host ? host : "", port,
             dial_host ? dial_host : "", dial_port,
             use_tls ? 1 : 0, insecure ? 1 : 0, cafile ? cafile : "");
}

/* Caller holds the lock. Drops every entry idle past the timeout.
 *
 * Returns immediately when the earliest deadline is still in the future, which
 * is the common case on a busy proxy -- see http_pool_next_expiry_ms. When it
 * does sweep, it recomputes that watermark from the survivors. */
static void http_pool_expire_locked(int64_t now) {
    if (now < http_pool_next_expiry_ms) return;

    int64_t earliest = INT64_MAX;
    HttpIdleConn** link = &http_pool_head;
    while (*link) {
        HttpIdleConn* c = *link;
        if (now - c->idle_since_ms >= http_pool_idle_ms) {
            *link = c->next;
            http_pool_count--;
            transport_close(&c->t);
            free(c);
            continue;
        }
        int64_t due = c->idle_since_ms + http_pool_idle_ms;
        if (due < earliest) earliest = due;
        link = &c->next;
    }
    http_pool_next_expiry_ms = earliest;
}

/* Take an idle connection for `key`, newest first (the most recently used is
 * the one the peer is least likely to have closed). Returns 1 on a hit. */
static int http_pool_take(const char* key, Transport* out) {
    if (!http_pool_enabled) return 0;
    int found = 0;
    pthread_mutex_lock(http_pool_lock());
    http_pool_expire_locked(http_now_ms());
    HttpIdleConn** link = &http_pool_head;
    while (*link) {
        HttpIdleConn* c = *link;
        if (strcmp(c->key, key) == 0) {
            *link = c->next;
            http_pool_count--;
            *out = c->t;
            free(c);
            found = 1;
            break;
        }
        link = &c->next;
    }
    pthread_mutex_unlock(http_pool_lock());
    return found;
}

/* Hand a still-usable connection back. Takes ownership either way: over the
 * caps it is closed here rather than returned to the caller to close. */
static void http_pool_put(const char* key, Transport* t) {
    if (!http_pool_enabled || t->sockfd < 0) {
        transport_close(t);
        return;
    }
    HttpIdleConn* c = (HttpIdleConn*)malloc(sizeof(HttpIdleConn));
    if (!c) {
        transport_close(t);
        return;
    }
    int64_t now = http_now_ms();
    pthread_mutex_lock(http_pool_lock());
    http_pool_expire_locked(now);
    /* Only whether the per-key cap is REACHED matters, not the exact count, so
     * stop at the cap instead of walking to the end -- and skip the walk
     * entirely when the global cap already rejects this connection. */
    int per_key = 0;
    if (http_pool_count < http_pool_max_idle) {
        for (HttpIdleConn* e = http_pool_head; e; e = e->next) {
            if (strcmp(e->key, key) == 0 && ++per_key >= http_pool_max_per_key)
                break;
        }
    }
    if (http_pool_count >= http_pool_max_idle || per_key >= http_pool_max_per_key) {
        pthread_mutex_unlock(http_pool_lock());
        free(c);
        transport_close(t);
        return;
    }
    c->t = *t;
    snprintf(c->key, sizeof(c->key), "%s", key);
    c->idle_since_ms = now;
    c->next = http_pool_head;
    http_pool_head = c;
    http_pool_count++;
    /* On an empty pool the watermark is INT64_MAX, and this entry is now the
     * only deadline there is. Lowering it here is what re-arms the sweep. */
    if (c->idle_since_ms + http_pool_idle_ms < http_pool_next_expiry_ms)
        http_pool_next_expiry_ms = c->idle_since_ms + http_pool_idle_ms;
    pthread_mutex_unlock(http_pool_lock());
    t->sockfd = -1;
#ifdef AETHER_HAS_OPENSSL
    t->ssl = NULL;
    t->owned_ctx = NULL;
#endif
}

/* Close and drop every idle connection. */
void http_client_pool_clear_raw(void) {
    pthread_mutex_lock(http_pool_lock());
    HttpIdleConn* c = http_pool_head;
    http_pool_head = NULL;
    http_pool_count = 0;
    http_pool_next_expiry_ms = INT64_MAX;   /* nothing left to expire */
    pthread_mutex_unlock(http_pool_lock());
    while (c) {
        HttpIdleConn* next = c->next;
        transport_close(&c->t);
        free(c);
        c = next;
    }
}

/* Reconfigure the pool. `max_idle` 0 turns reuse off (and clears what is
 * held); negative values leave that setting untouched. */
const char* http_client_pool_configure_raw(int max_idle, int max_per_host,
                                           int64_t idle_ns) {
    pthread_mutex_lock(http_pool_lock());
    if (max_idle >= 0) {
        http_pool_max_idle = max_idle;
        http_pool_enabled = max_idle > 0;
    }
    if (max_per_host >= 0) http_pool_max_per_key = max_per_host;
    if (idle_ns >= 0) {
        int64_t ms = idle_ns / 1000000LL;
        http_pool_idle_ms = ms > 0 ? ms : 1;
        /* The expiry watermark was derived from the PREVIOUS window, so
         * shortening the window would leave a deadline further out than the new
         * setting allows and delay every eviction. Force the next pool
         * operation to sweep and recompute it. */
        http_pool_next_expiry_ms = INT64_MIN;
    }
    pthread_mutex_unlock(http_pool_lock());
    if (!http_pool_enabled) http_client_pool_clear_raw();
    return "";
}

int http_client_pool_idle_count_raw(void) {
    pthread_mutex_lock(http_pool_lock());
    int n = http_pool_count;
    pthread_mutex_unlock(http_pool_lock());
    return n;
}


static void http_stream_free(struct HttpStream* s) {
    if (!s) return;
    transport_close(&s->t);
    free(s->pending);
    free(s);
}

/* Compact the unconsumed region to the front of `pending`, then read one more
 * window from the transport onto the end. Returns the unconsumed byte count
 * afterward: >0 = have data, 0 = clean EOF (nothing more will arrive),
 * -1 = transport error (sets s->err). */
static int stream_pump(struct HttpStream* s) {
    int avail = s->pending_len - s->pending_pos;
    if (s->pending_pos > 0) {
        if (avail > 0) memmove(s->pending, s->pending + s->pending_pos, (size_t)avail);
        s->pending_len = avail;
        s->pending_pos = 0;
    }
    if (s->pending_cap - s->pending_len < 8192) {
        int ncap = s->pending_cap ? s->pending_cap * 2 : 16384;
        while (ncap - s->pending_len < 8192) ncap *= 2;
        char* nb = (char*)realloc(s->pending, (size_t)ncap);
        if (!nb) { s->err = 1; return -1; }
        s->pending = nb;
        s->pending_cap = ncap;
    }
    int n = transport_recv(&s->t, s->pending + s->pending_len,
                           s->pending_cap - s->pending_len);
    if (n < 0) { s->err = 1; return -1; }
    if (n > 0) s->pending_len += n;
    return s->pending_len - s->pending_pos;  /* n==0 -> whatever remains buffered */
}

/* Consume a CRLF (or bare LF, tolerated) at the current read position, pumping
 * if needed. Returns 1 on success, 0 if a line terminator can't be obtained
 * (malformed framing at EOF -> sets s->err). */
static int stream_consume_crlf(struct HttpStream* s) {
    for (;;) {
        int avail = s->pending_len - s->pending_pos;
        if (avail >= 1) {
            char c = s->pending[s->pending_pos];
            if (c == '\r') {
                if (avail >= 2) {
                    s->pending_pos += (s->pending[s->pending_pos + 1] == '\n') ? 2 : 1;
                    return 1;
                }
                /* need the byte after '\r' */
            } else if (c == '\n') {
                s->pending_pos += 1;
                return 1;
            } else {
                /* not a line terminator where one was expected */
                s->err = 1;
                return 0;
            }
        }
        int r = stream_pump(s);
        if (r < 0) return 0;
        if (r == avail) { s->err = 1; return 0; }  /* EOF, no terminator */
    }
}

/* Parse a chunk-size line ("<hex>[;ext]\r\n"), pumping until the CRLF is
 * available. Sets *out to the parsed size and advances past the line.
 * Returns 1 on success, 0 on malformed framing / EOF (sets s->err). */
static int stream_read_chunk_size(struct HttpStream* s, long long* out) {
    for (;;) {
        /* find CRLF in the unconsumed region */
        int start = s->pending_pos, end = s->pending_len, nl = -1;
        for (int i = start; i < end; i++) {
            if (s->pending[i] == '\n') { nl = i; break; }
        }
        if (nl >= 0) {
            long long sz = 0; int any = 0;
            for (int i = start; i < nl; i++) {
                char c = s->pending[i];
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;  /* end of hex digits (CR, ';', or ext) */
                sz = sz * 16 + d;
                any = 1;
            }
            s->pending_pos = nl + 1;  /* consume through the LF */
            if (!any) { s->err = 1; return 0; }
            *out = sz;
            return 1;
        }
        int prev = s->pending_len - s->pending_pos;
        int r = stream_pump(s);
        if (r < 0) return 0;
        if (r == prev) { s->err = 1; return 0; }  /* EOF without a size line */
    }
}

/* Deliver up to `max` decoded body bytes into `out`. Returns bytes delivered
 * (>0), 0 at clean end-of-body, or -1 on transport / framing error. */
static int stream_read_decoded(struct HttpStream* s, char* out, int max) {
    if (s->err) return -1;
    if (s->eof) return 0;
    if (max <= 0) return 0;

    if (!s->chunked) {
        int avail = s->pending_len - s->pending_pos;
        if (avail == 0) {
            int r = stream_pump(s);
            if (r < 0) return -1;
            avail = s->pending_len - s->pending_pos;
            if (avail == 0) { s->eof = 1; return 0; }  /* EOF (or content-length short) */
        }
        int give = avail < max ? avail : max;
        if (!s->read_until_close && (long long)give > s->content_remaining)
            give = (int)s->content_remaining;
        memcpy(out, s->pending + s->pending_pos, (size_t)give);
        s->pending_pos += give;
        if (!s->read_until_close) {
            s->content_remaining -= give;
            if (s->content_remaining <= 0) s->eof = 1;
        }
        return give;
    }

    /* Chunked: run the state machine until `out` fills or the body ends. */
    int produced = 0;
    while (produced < max && !s->eof) {
        if (s->chunk_state == 1) {  /* deliver chunk payload */
            if (s->chunk_remaining == 0) { s->chunk_state = 2; continue; }
            int avail = s->pending_len - s->pending_pos;
            if (avail == 0) {
                int r = stream_pump(s);
                if (r < 0) return produced ? produced : -1;
                avail = s->pending_len - s->pending_pos;
                if (avail == 0) { s->err = 1; return produced ? produced : -1; }  /* truncated */
            }
            int give = avail;
            if ((long long)give > s->chunk_remaining) give = (int)s->chunk_remaining;
            if (give > max - produced) give = max - produced;
            memcpy(out + produced, s->pending + s->pending_pos, (size_t)give);
            s->pending_pos += give;
            s->chunk_remaining -= give;
            produced += give;
            if (s->chunk_remaining == 0) s->chunk_state = 2;
            continue;
        }
        if (s->chunk_state == 2) {  /* trailing CRLF after chunk data */
            if (!stream_consume_crlf(s)) return produced ? produced : -1;
            s->chunk_state = 0;
            continue;
        }
        /* chunk_state == 0: read the next size line */
        long long sz = 0;
        if (!stream_read_chunk_size(s, &sz)) return produced ? produced : -1;
        if (sz == 0) {
            /* terminal chunk: consume the CRLF after "0" (trailers, if any,
             * are ignored) and end the body. */
            stream_consume_crlf(s);
            s->eof = 1;
            break;
        }
        s->chunk_remaining = sz;
        s->chunk_state = 1;
    }
    return produced;
}

// -----------------------------------------------------------------
// v2 request builder — opaque struct + per-field setters. The v1
// one-liners (http_get_raw / http_post_raw / etc.) build a request
// internally and call http_send_raw, so all paths funnel through
// the same socket / TLS code below.
// -----------------------------------------------------------------

/* One allocation per header, not three (#1739).
 *
 * `name` and `value` point into the same block as the node, laid out
 * immediately after it: [HttpHeader][name\0][value\0]. A proxied request
 * forwards on the order of seven headers, and at a calloc plus two strdups
 * each that was 21 of the 76 allocations a proxied request made. They are
 * written once at insertion and never reassigned, so there is nothing to
 * grow and the node's own free() releases all three.
 *
 * The pointers stay rather than being computed from the node, because every
 * reader is `h->name` / `h->value` and this keeps them working unchanged. */
typedef struct HttpHeader {
    char* name;
    char* value;
    struct HttpHeader* next;
} HttpHeader;

struct HttpClientRequest {
    char* method;        /* "GET", "POST", etc. — owned, NUL-terminated */
    char* url;           /* full URL — owned, NUL-terminated */
    HttpHeader* headers; /* singly-linked, in insertion order */
    HttpHeader* headers_tail; /* last node, so insertion is O(1) (#1739) */
    char* body;          /* may be NULL; binary-safe via body_len */
    int   body_len;      /* explicit length; 0 if no body */
    char* content_type;  /* may be NULL; defaults applied at send time */
    int64_t timeout_ns;  /* 0 = no timeout (block forever). Whole-ns
                          * precision; the platform timeout calls
                          * (select tv, SO_RCVTIMEO timeval, Winsock
                          * DWORD ms) slice this down to whatever
                          * granularity they support. */
    int   max_redirects; /* 0 = don't follow (default); N>0 = follow up to N hops */
    int   insecure;      /* 0 = verify peer cert + hostname (default); 1 = skip
                          * both for THIS connection only (curl -k equivalent).
                          * Relaxed per-SSL, never on the shared SSL_CTX. */
    char* cafile;        /* owned, NUL-terminated PEM path, or NULL. When set
                          * (and !insecure), verify the peer against THIS CA
                          * bundle instead of the system store — pin a private/
                          * self-signed CA while KEEPING peer + hostname
                          * verification on. Applied per-connection via a
                          * per-SSL X509_STORE, never on the shared SSL_CTX
                          * (#1107). */
    int   stream;        /* 0 = buffer the whole body (default); 1 = streaming:
                          * read only the header block and hand the open
                          * transport to an HttpStream for incremental reads (#1004). */
    /* Forward-proxy control. Default is DIRECT (no proxy, env ignored) — the
     * hardened stance that avoids the httpoxy (CVE-2016-5385) ambient-authority
     * footgun. Precedence, highest first:
     *   PROXY_MODE_IGNORE (3)   force direct, ignore everything
     *   PROXY_MODE_EXPLICIT (2) use proxy_url, env ignored entirely
     *   PROXY_MODE_ENV (1)      follow HTTP(S)_PROXY/NO_PROXY, guarded
     *   PROXY_MODE_DIRECT (0)   default: no proxy */
    int   proxy_mode;
    char* proxy_url;     /* owned; set only for PROXY_MODE_EXPLICIT */
};

/* Proxy-mode ladder (see struct HttpClientRequest.proxy_mode). */
#define PROXY_MODE_DIRECT   0
#define PROXY_MODE_ENV      1
#define PROXY_MODE_EXPLICIT 2
#define PROXY_MODE_IGNORE   3

/* A header name has to be a token, and a value has to be free of the bytes
 * that end a line.
 *
 * Without this, a value carrying CR LF is written into the request head
 * verbatim and becomes additional headers: a caller that builds a value out
 * of anything a user supplied hands that user the rest of the request, and a
 * doubled CR LF ends the head entirely and starts a second request the peer
 * will answer (CWE-93). The header is rejected rather than repaired, because
 * silently sending something other than what was asked for is its own bug.
 */
int http_header_name_ok(const char* name) {
    if (!name || !*name) return 0;
    for (const unsigned char* c = (const unsigned char*)name; *c; c++) {
        /* RFC 9110 token: these punctuation marks, plus letters and digits. */
        if (!(isalnum(*c) || strchr("!#$%&'*+-.^_`|~", (int)*c))) return 0;
    }
    return 1;
}

int http_header_value_ok(const char* value) {
    if (!value) return 0;
    for (const unsigned char* c = (const unsigned char*)value; *c; c++) {
        if (*c == '\r' || *c == '\n') return 0;
    }
    return 1;
}

HttpClientRequest* http_request_raw(const char* method, const char* url) {
    if (!url || !*url) return NULL;
    /* The method is a token and the URL cannot carry a line ending, for the
     * same reason a header value cannot: both are written into the request
     * line, so a CR LF in either ends that line early and everything after it
     * is read by the peer as headers of its own (CWE-93). A URL built from
     * anything a user supplied is the ordinary way this happens. */
    if (!http_header_name_ok(method)) return NULL;
    if (!http_header_value_ok(url)) return NULL;
    HttpClientRequest* req = (HttpClientRequest*)calloc(1, sizeof(HttpClientRequest));
    if (!req) return NULL;
    req->method = strdup(method);
    req->url    = strdup(url);
    if (!req->method || !req->url) {
        http_request_free_raw(req);
        return NULL;
    }
    return req;
}

int http_request_set_header_raw(HttpClientRequest* req, const char* name, const char* value) {
    if (!req) return -1;
    if (!http_header_name_ok(name) || !http_header_value_ok(value)) return -1;
    size_t nl = strlen(name), vl = strlen(value);
    HttpHeader* h = (HttpHeader*)malloc(sizeof(HttpHeader) + nl + 1 + vl + 1);
    if (!h) return -1;
    h->name = (char*)(h + 1);
    memcpy(h->name, name, nl + 1);
    h->value = h->name + nl + 1;
    memcpy(h->value, value, vl + 1);
    h->next = NULL;
    /* Append at the tail so emission order matches insertion order;
     * keeps tests deterministic and avoids surprises with servers
     * that care about header ordering (rare but exists). The tail is
     * tracked rather than walked to: appending N headers by walking cost
     * N(N+1)/2 traversals for a list that is only ever appended to. */
    if (req->headers_tail) {
        req->headers_tail->next = h;
    } else {
        req->headers = h;
    }
    req->headers_tail = h;
    return 0;
}

int http_request_set_body_raw(HttpClientRequest* req, const char* body, int len, const char* content_type) {
    if (!req || len < 0) return -1;
    /* Replace any prior body. Cap-aware (#343): req->body_len holds
     * the original allocation size and is reset to 0 below before
     * the next aether_caps_malloc. */
    aether_caps_free(req->body, (size_t)req->body_len);
    req->body = NULL; req->body_len = 0;
    free(req->content_type); req->content_type = NULL;
    if (len > 0) {
        if (!body) return -1;
        /* `body` may be either an AetherString* (when the caller
         * passed an Aether string variable — common for binary
         * payloads from fs.read_binary) or a plain char* (string
         * literals). Without this unwrap, a 10-byte AetherString
         * input would copy the 24-byte struct header (magic +
         * refcount + length + capacity + data-ptr) into our body
         * buffer, and the wire would carry that header. Same shape
         * the std.fs / std.cryptography / std.zlib externs use. */
        const char* src = body;
        if (is_aether_string(body)) {
            src = ((const AetherString*)body)->data;
        }
        /* Cap-aware (#343): caller-supplied length, untrusted on
         * --emit=lib paths. The matching aether_caps_free passes
         * req->body_len as the size. */
        req->body = (char*)aether_caps_malloc((size_t)len);
        if (!req->body) return -1;
        memcpy(req->body, src, (size_t)len);
        req->body_len = len;
    }
    if (content_type) {
        req->content_type = strdup(content_type);
        if (!req->content_type) return -1;
    }
    return 0;
}

int http_request_set_timeout_raw(HttpClientRequest* req, int seconds) {
    if (!req || seconds < 0) return -1;
    req->timeout_ns = (int64_t)seconds * 1000000000LL;
    return 0;
}

int http_request_set_timeout_ns_raw(HttpClientRequest* req, int64_t timeout_ns) {
    if (!req || timeout_ns < 0) return -1;
    req->timeout_ns = timeout_ns;
    return 0;
}

int http_request_set_follow_redirects_raw(HttpClientRequest* req, int max_hops) {
    if (!req || max_hops < 0) return -1;
    req->max_redirects = max_hops;
    return 0;
}

/* Skip TLS peer + hostname verification for this request only (curl -k /
 * wget --no-check-certificate). `on` non-zero enables it; 0 (the default)
 * verifies. The relaxation is applied per-SSL in the TLS path below, never on
 * the process-wide SSL_CTX singleton — so one insecure request cannot silently
 * downgrade verification for every other request in the process. */
int http_request_set_insecure_raw(HttpClientRequest* req, int on) {
    if (!req) return -1;
    req->insecure = on ? 1 : 0;
    return 0;
}

/* Pin a custom CA for THIS request: verify the peer certificate against the PEM
 * bundle at `path` instead of the system trust store, while keeping peer and
 * hostname verification ON (#1107). This is the "verify, but against THIS cert"
 * knob — strictly stronger than set_insecure, for machine-to-machine calls to a
 * host with a private/self-signed CA (e.g. a Proxmox VE API). Applied
 * per-connection in the TLS path via a per-SSL X509_STORE, never on the shared
 * SSL_CTX. Passing NULL/empty clears the pin (revert to the system store).
 * `path` is copied. Returns 0, or -1 on a null request. */
int http_request_set_cafile_raw(HttpClientRequest* req, const char* path) {
    if (!req) return -1;
    free(req->cafile);
    req->cafile = (path && *path) ? strdup(path) : NULL;
    return 0;
}

/* #1004: enable streaming response bodies for THIS request. When on, the
 * response returned by http_send_raw carries an open HttpStream instead of a
 * buffered body; the caller pulls the body via http_response_read_chunk_raw
 * and must free the response (which closes the transport) when done. */
int http_request_set_stream_raw(HttpClientRequest* req, int on) {
    if (!req) return -1;
    req->stream = on ? 1 : 0;
    return 0;
}

/* ---- Forward-proxy control (aether#1012 part 2) ------------------------------
 *
 * Default is DIRECT: std.http.client does NOT follow $HTTP_PROXY unless the
 * program opts in. This is the deliberate inverse of Go/PHP/Python's original
 * default-follow, which produced the httpoxy vulnerability class
 * (CVE-2016-5385): a CGI/serverless environment maps an attacker-controlled
 * `Proxy:` request header to $HTTP_PROXY, silently redirecting outbound
 * traffic. Hardened-by-default means there is no ambient env-follow path to
 * exploit; env-following is an explicit verb, not a default anyone falls into.
 *
 * Precedence (highest first): ignore > explicit > env > direct. */

/* use_env_proxy: follow $HTTP_PROXY/$HTTPS_PROXY/$NO_PROXY (Go-compatible),
 * WITH the httpoxy + SSRF guards applied at connect time (see
 * resolve_proxy_for). `on` non-zero enables; 0 reverts to direct. */
int http_request_use_env_proxy_raw(HttpClientRequest* req, int on) {
    if (!req) return -1;
    req->proxy_mode = on ? PROXY_MODE_ENV : PROXY_MODE_DIRECT;
    return 0;
}

/* use_http_proxy: pin an explicit proxy (`http://host:port`). This OVERRIDES
 * env entirely — $HTTP_PROXY is never consulted — so a team-controlled proxy
 * (recorder / toxiproxy) is immune to whatever the shell/CI has set. Passing
 * an empty/NULL url reverts to direct. */
int http_request_use_http_proxy_raw(HttpClientRequest* req, const char* proxy_url) {
    if (!req) return -1;
    free(req->proxy_url);
    req->proxy_url = NULL;
    if (!proxy_url || !*proxy_url) {
        req->proxy_mode = PROXY_MODE_DIRECT;
        return 0;
    }
    req->proxy_url = strdup(proxy_url);
    if (!req->proxy_url) return -1;
    req->proxy_mode = PROXY_MODE_EXPLICIT;
    return 0;
}

/* ignore_http_proxy: force a direct connection regardless of env or any proxy
 * a higher layer set. The determinism escape hatch (e.g. VCR record mode that
 * must capture the origin, not a proxy's view). */
int http_request_ignore_http_proxy_raw(HttpClientRequest* req) {
    if (!req) return -1;
    free(req->proxy_url);
    req->proxy_url = NULL;
    req->proxy_mode = PROXY_MODE_IGNORE;
    return 0;
}

void http_request_free_raw(HttpClientRequest* req) {
    if (!req) return;
    free(req->method);
    free(req->url);
    /* Body was alloc'd via aether_caps_malloc in
     * http_request_set_body_raw; pair the free with the recorded
     * length so cap accounting stays at current-usage. */
    aether_caps_free(req->body, (size_t)req->body_len);
    free(req->content_type);
    free(req->proxy_url);
    free(req->cafile);
    HttpHeader* h = req->headers;
    while (h) {
        HttpHeader* next = h->next;
        free(h);          /* name and value live in this same block */
        h = next;
    }
    req->headers = NULL;
    req->headers_tail = NULL;
    free(req);
}

/* Forward decls — defined below the static request() function. */
static int header_already_set(const HttpClientRequest* req, const char* name);
static char* http_extract_response_header(const char* hdr_block, const char* name);
/* Decode HTTP/1.1 chunked transfer-encoding. Returns a malloc'd decoded
 * buffer (NUL-terminated; *out_len excludes the NUL) or NULL on malformed
 * framing (caller then keeps the raw body). Binary-safe (copies by
 * length). Defined below. */
static int http_value_has_chunked(const char* v);
static char* http_extract_response_header(const char* hdr_block, const char* name);

/* Is the chunked body in `buf` complete, i.e. has the terminating zero-size
 * chunk arrived? Walks chunk headers rather than searching for "0\r\n\r\n",
 * which can appear inside chunk data. */
/* How many bytes does the complete chunked body at `buf` occupy, counting its
 * framing and trailers, or 0 when it is not complete yet? A complete body is
 * never zero-length (the terminal chunk alone is five bytes), so 0 is an
 * unambiguous "not yet".
 *
 * The length matters as much as the fact: anything after it belongs to
 * whatever the peer sent next, and a caller that assumes the body runs to the
 * end of what it has read would swallow a pipelined message. */
size_t http_chunked_frame_len(const char* buf, size_t len) {
    size_t off = 0;
    for (;;) {
        const char* line_end = (const char*)memchr(buf + off, '\n', len - off);
        if (!line_end) return 0;
        size_t size_len = (size_t)(line_end - (buf + off));
        char size_buf[32];
        if (size_len >= sizeof(size_buf)) return 0;
        memcpy(size_buf, buf + off, size_len);
        size_buf[size_len] = '\0';
        long long chunk = strtoll(size_buf, NULL, 16);
        if (chunk < 0) return 0;
        off = (size_t)(line_end - buf) + 1;
        if (chunk == 0) {
            /* Trailer section: header lines until an empty one ends it. */
            for (;;) {
                const char* le = (const char*)memchr(buf + off, '\n', len - off);
                if (!le) return 0;
                size_t line_len = (size_t)(le - (buf + off));
                int empty = line_len == 0 || (line_len == 1 && buf[off] == '\r');
                off = (size_t)(le - buf) + 1;
                if (empty) return off;
            }
        }
        off += (size_t)chunk + 2;   /* payload + its trailing CRLF */
        if (off > len) return 0;
    }
}

int http_chunked_complete(const char* buf, size_t len) {
    return http_chunked_frame_len(buf, len) > 0;
}

/* Case-insensitive substring search, for header values. */
static const char* http_strcasestr_local(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return NULL;
    size_t nlen = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            i++;
        }
        if (i == nlen) return p;
    }
    return NULL;
}

/* The status code from a response header block. */
/* The status code from a status line, or 0 when the line does not carry one.
 *
 * It is exactly three digits (RFC 9112 4). Reading it with atoi instead
 * accepts anything that starts with a digit and wraps on overflow, so an
 * upstream answering "HTTP/1.1 999999999999 Weird" handed the caller a status
 * of -727379969, and a proxy copied that onto the response it sent back. */
static int response_status_of(const char* header_block) {
    const char* sp = strchr(header_block, ' ');
    if (!sp) return 0;
    const unsigned char* d = (const unsigned char*)sp + 1;
    if (!isdigit(d[0]) || !isdigit(d[1]) || !isdigit(d[2])) return 0;
    if (isdigit(d[3])) return 0;
    return (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
}

/* RFC 9110: 1xx, 204 and 304 carry no body, and neither does any response to
 * HEAD, whatever the headers say. */
static int no_body_expected(int status, const char* method) {
    if (method && strcmp(method, "HEAD") == 0) return 1;
    if (status >= 100 && status < 200) return 1;
    return status == 204 || status == 304;
}

// -----------------------------------------------------------------
// Forward-proxy resolution (aether#1012 part 2)
// -----------------------------------------------------------------

/* Is `env` a case-insensitive key present and non-empty? Returns its value or
 * NULL. Checks both cases (HTTP_PROXY and http_proxy). */
static const char* proxy_env(const char* upper, const char* lower) {
    const char* v = getenv(lower);          /* lowercase preferred (curl-compat) */
    if (v && *v) return v;
    v = getenv(upper);
    return (v && *v) ? v : NULL;
}

/* httpoxy (CVE-2016-5385) guard: under a CGI/serverless invocation the
 * `Proxy:` request header is mapped to $HTTP_PROXY, so an attacker can inject
 * a proxy. Detect that context and refuse the UPPERCASE HTTP_PROXY there (the
 * lowercase http_proxy is not settable via the CGI header map, so it stays
 * usable — same split Go's net/http/httpproxy adopted). */
static int running_under_cgi(void) {
    return getenv("REQUEST_METHOD") != NULL || getenv("GATEWAY_INTERFACE") != NULL;
}

/* SSRF hardening: reject a proxy that points at a loopback / link-local /
 * cloud-metadata address, so a stray or hostile env var can't redirect
 * outbound traffic to 127.0.0.1, 169.254.169.254 (IMDS), etc. Returns 1 if the
 * literal IP host is dangerous. Hostnames (not IP literals) pass — resolving
 * and re-checking every A record is a heavier follow-up; the common exploit
 * uses a bare IP. */
static int proxy_host_is_dangerous(const char* host) {
    if (!host || !*host) return 1;
    struct in_addr a4;
    if (inet_pton(AF_INET, host, &a4) == 1) {
        uint32_t h = ntohl(a4.s_addr);
        if ((h >> 24) == 127) return 1;                 /* 127.0.0.0/8 loopback */
        if ((h >> 24) == 0)   return 1;                 /* 0.0.0.0/8 */
        if ((h & 0xFFFF0000u) == 0xA9FE0000u) return 1; /* 169.254.0.0/16 link-local (IMDS) */
        return 0;
    }
#ifdef AF_INET6
    struct in6_addr a6;
    if (inet_pton(AF_INET6, host, &a6) == 1) {
        static const unsigned char loop[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (memcmp(&a6, loop, 16) == 0) return 1;       /* ::1 */
        if ((a6.s6_addr[0] & 0xFE) == 0xFC) return 1;   /* fc00::/7 ULA */
        if (a6.s6_addr[0] == 0xFE && (a6.s6_addr[1] & 0xC0) == 0x80) return 1; /* fe80::/10 */
        return 0;
    }
#endif
    return 0; /* a hostname; allowed */
}

/* Does `target_host` match any entry of a NO_PROXY list ("host,.dom,10.0.0.1")?
 * Suffix match on a leading dot; exact otherwise; "*" matches all. */
static int host_in_no_proxy(const char* target_host, const char* no_proxy) {
    if (!no_proxy || !*no_proxy || !target_host) return 0;
    size_t tlen = strlen(target_host);
    const char* p = no_proxy;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char* start = p;
        while (*p && *p != ',') p++;
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t elen = (size_t)(end - start);
        if (elen == 1 && start[0] == '*') return 1;
        if (elen > 0) {
            if (start[0] == '.') {              /* ".example.com" → suffix */
                if (tlen >= elen &&
                    strncasecmp(target_host + (tlen - elen), start, elen) == 0) return 1;
            } else if (elen == tlen &&
                       strncasecmp(target_host, start, elen) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* Resolve the effective proxy for a request+target, applying the precedence
 * ladder and all guards. On "use a proxy" writes proxy_host/proxy_port and
 * returns 1; returns 0 for a direct connection; returns -1 with *err set on a
 * rejected/malformed proxy (caller surfaces it). */
static int resolve_proxy_for(HttpClientRequest* req, const char* target_host, int use_tls,
                             char* proxy_host, size_t phost_size, int* proxy_port,
                             const char** err) {
    *err = NULL;
    if (req->proxy_mode == PROXY_MODE_IGNORE || req->proxy_mode == PROXY_MODE_DIRECT) {
        return 0;
    }

    const char* proxy_url = NULL;
    if (req->proxy_mode == PROXY_MODE_EXPLICIT) {
        proxy_url = req->proxy_url;             /* env-immune, no guards beyond parse */
    } else { /* PROXY_MODE_ENV */
        const char* no_proxy = proxy_env("NO_PROXY", "no_proxy");
        if (host_in_no_proxy(target_host, no_proxy)) return 0;
        if (use_tls) {
            proxy_url = proxy_env("HTTPS_PROXY", "https_proxy");
        } else {
            /* httpoxy: only the UPPERCASE HTTP_PROXY is CGI-injectable; refuse
             * it under CGI, still honor lowercase http_proxy. */
            const char* lower = getenv("https_proxy"); (void)lower;
            const char* lp = getenv("http_proxy");
            const char* up = getenv("HTTP_PROXY");
            if (lp && *lp)               proxy_url = lp;
            else if (up && *up && !running_under_cgi()) proxy_url = up;
            else                          proxy_url = NULL;
        }
        if (!proxy_url) return 0;
    }

    /* Parse proxy URL: [http://]host[:port]. Default port 3128. */
    const char* s = proxy_url;
    if (strncmp(s, "http://", 7) == 0) s += 7;
    else if (strncmp(s, "https://", 8) == 0) s += 8; /* proxy-over-TLS not supported; treat as host */
    int pport = 3128;
    const char* slash = strchr(s, '/');
    const char* colon = strchr(s, ':');
    size_t hlen;
    if (colon && (!slash || colon < slash)) {
        hlen = (size_t)(colon - s);
        pport = atoi(colon + 1);
    } else if (slash) {
        hlen = (size_t)(slash - s);
    } else {
        hlen = strlen(s);
    }
    if (hlen == 0 || hlen >= phost_size || pport <= 0 || pport > 65535) {
        *err = "malformed proxy URL";
        return -1;
    }
    memcpy(proxy_host, s, hlen);
    proxy_host[hlen] = '\0';

    /* SSRF guard applies to the ENV path (untrusted); an explicit
     * use_http_proxy is a code-visible grant, so it is trusted as-is. */
    if (req->proxy_mode == PROXY_MODE_ENV && proxy_host_is_dangerous(proxy_host)) {
        *err = "proxy from environment points at a loopback/link-local address (blocked)";
        return -1;
    }
    *proxy_port = pport;
    return 1;
}

// -----------------------------------------------------------------
// Core request, operating on an HttpClientRequest. v1 wrappers build a
// throwaway HttpClientRequest and discard it after send.
// -----------------------------------------------------------------

/* Send / receive timeouts for this request.
 *
 * SO_RCVTIMEO / SO_SNDTIMEO take different shapes on the two families: POSIX
 * a `struct timeval`, Winsock a DWORD of milliseconds. Passing a timeval to
 * Winsock makes it read tv_sec as milliseconds, so `set_timeout(35)` would
 * become a 35ms recv timeout that fires before any upstream can answer.
 * Both shapes keep sub-second precision; Winsock rounds up so a sub-ms value
 * cannot land on 0, which it reads as "infinite". */
static void http_apply_timeouts(int sockfd, int64_t timeout_ns) {
    if (timeout_ns < 0) timeout_ns = 0;   /* 0 clears: block indefinitely */
#ifdef _WIN32
    int64_t ms_total = (timeout_ns + 999999LL) / 1000000LL;
    if (ms_total > 0xFFFFFFFFLL) ms_total = 0xFFFFFFFFLL;
    DWORD rwtv_ms = (DWORD)ms_total;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rwtv_ms, sizeof(rwtv_ms));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&rwtv_ms, sizeof(rwtv_ms));
#else
    struct timeval rwtv;
    rwtv.tv_sec  = (time_t)(timeout_ns / 1000000000LL);
    rwtv.tv_usec = (long)((timeout_ns / 1000LL) % 1000000LL);
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rwtv, sizeof(rwtv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&rwtv, sizeof(rwtv));
#endif
}

/* Apply timeouts only when they differ from what the socket already carries.
 *
 * The unguarded http_apply_timeouts stays for the dial path, where the socket
 * is new and its option state is genuinely unknown. This form is for the reuse
 * path, where the answer is usually "already correct" -- see the comment on
 * Transport::applied_timeout_ns for the measurement. */
static void transport_apply_timeouts(Transport* t, int64_t timeout_ns) {
    int64_t want = timeout_ns < 0 ? 0 : timeout_ns;
    if (t->applied_timeout_ns == want) return;
    http_apply_timeouts(t->sockfd, want);
    t->applied_timeout_ns = want;
}

/* Resolve `host`:`port` into `out`, at most once per request (#1719).
 *
 * `*resolved` is the caller's once-flag: 0 on entry means "not yet", and it is
 * set on success so a second call is free. Returns 1 on success (including the
 * cached case), 0 when the name does not resolve.
 *
 * Resolve via getaddrinfo, NOT gethostbyname: gethostbyname returns a pointer
 * into a shared, process-static `struct hostent`, so two client calls resolving
 * at once on different threads -- e.g. a request handler that dials out while
 * serving (serve-and-dial), where the inner call runs on a server worker thread
 * -- race on that static buffer and can corrupt each other's resolved address.
 * getaddrinfo is thread-safe and returns caller-owned memory freed with
 * freeaddrinfo. Pinned to AF_INET: the callers build a sockaddr_in and the
 * timeout/connect path assumes IPv4, so widening to IPv6 is a separate change.
 *
 * A failed resolve leaves *resolved at 0, so a later caller retries rather than
 * reading an uninitialised address. */
static int resolve_dial_addr(const char* host, int port,
                             struct sockaddr_in* out, int* resolved) {
    if (*resolved) return 1;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return 0;

    memset(out, 0, sizeof(*out));
    memcpy(out, res->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(res);
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    *resolved = 1;
    return 1;
}

/* Set *err to a heap copy of `msg`, for the dial helper's error returns. */
static void ae_set_err(char** out_err, const char* msg) {
    if (!out_err) return;
    *out_err = msg ? strdup(msg) : NULL;
}

/* Open one connection to `serv_addr`: socket, connect (with the request's
 * timeout when set), the forward-proxy CONNECT tunnel for HTTPS, and the TLS
 * handshake. On success `out` holds a ready transport and 0 is returned; on
 * failure nothing is left open, *err holds a message the caller frees, and
 * the return is -1.
 *
 * Split out of http_request_internal so a pooled connection that turns out to
 * be dead can be redialled without duplicating any of this (#1653). */
static int http_dial(HttpClientRequest* req, struct sockaddr_in* serv_addr_in,
                     const char* host, int port, int use_tls, int via_proxy,
                     Transport* out, char** out_err) {
    struct sockaddr_in serv_addr = *serv_addr_in;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        ae_set_err(out_err, "could not create socket");
        return -1;
    }

    /* Connect — with timeout via non-blocking + select when the
     * caller asked for one. Without a timeout, fall through to the
     * original blocking connect (preserves v1 behaviour exactly). */
    int connect_rc;
    if (req->timeout_ns > 0) {
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sockfd, FIONBIO, &nb);
#else
        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags >= 0) fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
#endif
        connect_rc = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (connect_rc < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            int in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
            int in_progress = (errno == EINPROGRESS || errno == EWOULDBLOCK);
#endif
            if (in_progress) {
                /* Watch both the writable set (success) and the
                 * exception set (failure). On POSIX, a refused
                 * non-blocking connect makes the socket writable
                 * with SO_ERROR=ECONNREFUSED — only `wfds` matters.
                 * On Windows, Winsock signals connect failures via
                 * the *exception* fd set instead, NOT writable —
                 * so a select that watches only wfds waits the full
                 * timeout for refused connects rather than failing
                 * fast. Watching both makes select fire on either
                 * outcome; SO_ERROR distinguishes success from
                 * failure afterwards. */
#ifdef _WIN32
                /* Winsock's fd_set is a small array of SOCKET handles, not a
                 * bitmap indexed by descriptor, so it has no FD_SETSIZE
                 * hazard for a single socket. */
                fd_set wfds, efds;
                FD_ZERO(&wfds); FD_SET(sockfd, &wfds);
                FD_ZERO(&efds); FD_SET(sockfd, &efds);
                struct timeval tv;
                tv.tv_sec  = (time_t)(req->timeout_ns / 1000000000LL);
                tv.tv_usec = (long)((req->timeout_ns / 1000LL) % 1000000LL);
                int sel = select(sockfd + 1, NULL, &wfds, &efds, &tv);
#else
                /* poll, not select: a POSIX fd_set is a bitmap of FD_SETSIZE
                 * bits, and FD_SET on a descriptor at or above it writes past
                 * the end. A process serving many connections (a proxy, which
                 * is what dials most often) passes 1024 descriptors easily,
                 * and the corruption would land on the stack. poll takes the
                 * descriptor by value and has no such bound. POLLOUT covers
                 * both outcomes: a refused non-blocking connect reports the
                 * socket ready with SO_ERROR set, which the check below
                 * reads. */
                struct pollfd pfd;
                pfd.fd = sockfd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                int64_t timeout_ms = (req->timeout_ns + 999999LL) / 1000000LL;
                if (timeout_ms > INT_MAX) timeout_ms = INT_MAX;
                int sel = poll(&pfd, 1, (int)timeout_ms);
#endif
                if (sel == 0) {
                    close(sockfd);
                    ae_set_err(out_err, "connect timeout");
                    return -1;
                }
                if (sel < 0) {
                    close(sockfd);
                    ae_set_err(out_err, "select on connect failed");
                    return -1;
                }
                /* Check SO_ERROR — distinguishes "writable because
                 * connected" from "writable because failed". On
                 * Windows the failure surfaces via efds; on POSIX
                 * via wfds with so_err set. SO_ERROR works the
                 * same in both cases. */
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char*)&so_err, &slen) < 0
                    || so_err != 0) {
                    close(sockfd);
                    ae_set_err(out_err, "connection failed");
                    return -1;
                }
                connect_rc = 0;
            } else {
                close(sockfd);
                ae_set_err(out_err, "connection failed");
                return -1;
            }
        }
        /* Restore blocking mode for the send/recv path — those use
         * setsockopt(SO_*TIMEO) below for their timeouts. */
#ifdef _WIN32
        nb = 0;
        ioctlsocket(sockfd, FIONBIO, &nb);
#else
        if (flags >= 0) fcntl(sockfd, F_SETFL, flags);
#endif

        /* Apply send/recv timeouts equal to the configured value.
         *
         * SO_RCVTIMEO / SO_SNDTIMEO take different shapes on the two
         * families:
         *   - POSIX: pointer to `struct timeval` (seconds + microseconds)
         *   - Winsock: pointer to a 32-bit DWORD in milliseconds
         *
         * Passing a struct timeval to Winsock causes it to interpret
         * the first 4 bytes (tv_sec) as a millisecond count — so
         * `set_timeout(35)` would degrade to a 35-millisecond recv
         * timeout, which fires almost instantly and surfaces as
         * "recv timeout or I/O error" before any sane upstream can
         * even respond. Use the right type per platform.
         *
         * Both shapes carry sub-second precision: POSIX gets the full
         * microsecond resolution via tv_usec; Winsock rounds up to
         * the next whole millisecond (sub-ms DWORD=0 is ambiguous
         * with "infinite"). */
        http_apply_timeouts(sockfd, req->timeout_ns);
    } else {
        connect_rc = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (connect_rc < 0) {
            close(sockfd);
            ae_set_err(out_err, "connection failed");
            return -1;
        }
    }

    out->sockfd = sockfd;
    /* The dial path below applies the timeouts unconditionally on a fresh
     * socket; record the value so a later reuse can skip re-applying it. */
    out->applied_timeout_ns = req->timeout_ns < 0 ? 0 : req->timeout_ns;

    /* HTTPS via a forward proxy: establish a CONNECT tunnel over the raw socket
     * BEFORE the TLS handshake, so TLS runs end-to-end through the proxy (the
     * proxy is a blind pipe; it does not terminate TLS). HTTP via proxy needs no
     * tunnel — it uses an absolute-form request line, handled below. */
    if (via_proxy && use_tls) {
        char creq[512];
        int cn = snprintf(creq, sizeof(creq),
                          "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n"
                          "Proxy-Connection: keep-alive\r\n\r\n",
                          host, port, host, port);
        if (cn <= 0 || cn >= (int)sizeof(creq) ||
            send(sockfd, creq, (size_t)cn, 0) != cn) {
            close(sockfd);
            ae_set_err(out_err, "proxy CONNECT: send failed");
            return -1;
        }
        /* Read the proxy's response headers up to the terminating CRLFCRLF.
         * A 2xx means the tunnel is open; anything else is a proxy refusal. */
        char cbuf[1024];
        size_t clen = 0;
        int saw_end = 0;
        while (clen < sizeof(cbuf) - 1) {
            ssize_t rn = recv(sockfd, cbuf + clen, sizeof(cbuf) - 1 - clen, 0);
            if (rn <= 0) break;
            clen += (size_t)rn;
            cbuf[clen] = '\0';
            if (strstr(cbuf, "\r\n\r\n")) { saw_end = 1; break; }
        }
        if (!saw_end) {
            close(sockfd);
            ae_set_err(out_err, "proxy CONNECT: no response from proxy");
            return -1;
        }
        /* Parse "HTTP/1.x NNN ..." status. */
        int pstatus = 0;
        const char* sp = strchr(cbuf, ' ');
        if (sp) pstatus = atoi(sp + 1);
        if (pstatus < 200 || pstatus >= 300) {
            close(sockfd);
            char emsg[128];
            snprintf(emsg, sizeof(emsg),
                     "proxy CONNECT refused (status %d)", pstatus);
            ae_set_err(out_err, emsg);
            return -1;
        }
        /* Tunnel open. TLS handshake below runs against `host` end-to-end. */
    }

#ifdef AETHER_HAS_OPENSSL
    out->ssl = NULL;
    out->owned_ctx = NULL;

    if (use_tls) {
        /* Custom-CA pin (set_cafile): build a DEDICATED SSL_CTX whose trust
         * store is loaded from the couriered CA, instead of the shared
         * system-store CTX. Loading the CA onto the CTX's own verify store via
         * SSL_CTX_load_verify_locations is the portable, version-agnostic trust
         * idiom — it's exactly what `openssl s_client -CAfile` does, and it
         * verifies identically across OpenSSL 1.1/3.x and LibreSSL. (The prior
         * per-SSL SSL_set1_verify_cert_store approach verified on OpenSSL 3.x
         * but did not reliably become the *trust* store on every TLS library,
         * so a couriered CA that `openssl -CAfile` accepted still failed
         * `certificate verify failed` on some builds — #1110.) The per-request
         * CTX is owned by the transport and freed in transport_close after the
         * SSL. When no cafile is set we keep the shared process-wide CTX. */
        SSL_CTX* ctx;
        if (req->cafile) {
            ctx = SSL_CTX_new(TLS_client_method());
            if (ctx) {
                SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
                if (SSL_CTX_load_verify_locations(ctx, req->cafile, NULL) != 1) {
                    SSL_CTX_free(ctx);
                    close(sockfd);
                    char* msg = ssl_err_string("custom CA (set_cafile) load failed");
                    ae_set_err(out_err, msg ? msg : "custom CA load failed");
                    free(msg);
                    return -1;
                }
            }
            out->owned_ctx = ctx;   /* transport frees it (NULL is a no-op) */
        } else {
            ctx = get_ssl_ctx();
        }
        if (!ctx) {
            close(sockfd);
            char* msg = ssl_err_string("TLS context init failed");
            ae_set_err(out_err, msg ? msg : "TLS context init failed");
            free(msg);
            return -1;
        }

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            if (out->owned_ctx) { SSL_CTX_free(out->owned_ctx); out->owned_ctx = NULL; }
            close(sockfd);
            char* msg = ssl_err_string("SSL_new failed");
            ae_set_err(out_err, msg ? msg : "SSL_new failed");
            free(msg);
            return -1;
        }

        // SNI: server-name indication so virtual-hosted TLS services
        // return the right cert.
        SSL_set_tlsext_host_name(ssl, host);

        if (req->insecure) {
            // Per-request opt-out (set_insecure): skip peer verification AND
            // the hostname pin for THIS connection only. The shared SSL_CTX
            // (which set SSL_VERIFY_PEER) is untouched, so every other request
            // still verifies. Mirrors curl -k / wget --no-check-certificate.
            SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
        } else {
            // Verify the cert's CN/SAN matches the host we connected to. For an
            // IP literal (e.g. a Proxmox API at https://192.168.0.204:8006),
            // set1_ip_asc pins the IP SAN; for a DNS name, set1_host pins the
            // DNS SAN/CN. Using the IP-specific call for IP literals is correct
            // across OpenSSL versions (older set1_host did not auto-detect IPs).
            // The trust anchor for THIS connection is either the shared system
            // store or, when set_cafile was called, the couriered CA loaded onto
            // the per-request CTX above (#1107/#1110) — peer + hostname
            // verification stay ON either way, so a pinned request is strictly
            // stronger than set_insecure and fails closed if the CA doesn't
            // cover the presented cert.
            X509_VERIFY_PARAM* vpm = SSL_get0_param(ssl);
            X509_VERIFY_PARAM_set_hostflags(vpm, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            struct in_addr in4;
            struct in6_addr in6;
            if (inet_pton(AF_INET, host, &in4) == 1 ||
                inet_pton(AF_INET6, host, &in6) == 1) {
                X509_VERIFY_PARAM_set1_ip_asc(vpm, host);
            } else {
                X509_VERIFY_PARAM_set1_host(vpm, host, 0);
            }
        }

        SSL_set_fd(ssl, sockfd);
        int connect_result = SSL_connect(ssl);
        if (connect_result != 1) {
            int ssl_err = SSL_get_error(ssl, connect_result);
            (void)ssl_err;
            SSL_free(ssl);
            if (out->owned_ctx) { SSL_CTX_free(out->owned_ctx); out->owned_ctx = NULL; }
            close(sockfd);
            char* msg = ssl_err_string("TLS handshake failed");
            ae_set_err(out_err, msg ? msg : "TLS handshake failed");
            free(msg);
            return -1;
        }

        out->ssl = ssl;
    }
#endif
    return 0;
}


/* Find a header by name in a header block, anchored to the start of each line.
 *
 * Anchoring matters: a substring search over the whole block also matches
 * inside another header's value, so a message carrying
 * `X-Note: Content-Length: 9` would have had that read as its framing.
 *
 * Shared by the client and the server: a response frames its body the same way
 * a request does, and two scanners would be two chances to disagree.
 *
 * Returns how many times the header appears, writes the first value into
 * `out`, and sets *differing when two of them disagree. A framing header that
 * appears twice with two answers has no single answer at all.
 */
int http_find_header_in_block(const char* block, const char* end,
                            const char* name, char* out, size_t out_cap,
                            int* differing) {
    size_t name_len = strlen(name);
    int count = 0;
    if (differing) *differing = 0;
    if (out && out_cap) out[0] = '\0';

    for (const char* line = block; line && line < end; ) {
        const char* eol = (const char*)memchr(line, '\n', (size_t)(end - line));
        const char* line_end = eol ? eol : end;
        size_t line_len = (size_t)(line_end - line);
        if (line_len && line[line_len - 1] == '\r') line_len--;

        if (line_len > name_len && strncasecmp(line, name, name_len) == 0
            && line[name_len] == ':') {
            const char* v = line + name_len + 1;
            const char* v_end = line + line_len;
            while (v < v_end && (*v == ' ' || *v == '\t')) v++;
            while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t')) v_end--;
            size_t vl = (size_t)(v_end - v);

            char value[256];
            if (vl >= sizeof(value)) vl = sizeof(value) - 1;
            memcpy(value, v, vl);
            value[vl] = '\0';

            if (count == 0) {
                if (out && out_cap) {
                    size_t copy = vl < out_cap - 1 ? vl : out_cap - 1;
                    memcpy(out, value, copy);
                    out[copy] = '\0';
                }
            } else if (differing && out && strcmp(out, value) != 0) {
                *differing = 1;
            }
            count++;
        }
        if (!eol) break;
        line = eol + 1;
    }
    return count;
}

/* Has a complete HTTP/1.1 response accumulated in `buf`?
 *
 * Reading to end of connection only works when the connection is the
 * delimiter, which is what made every request cost a fresh one (#1653). This
 * reads to the end of THIS response: the header block, then exactly the body
 * its framing declares. `f` carries what has been worked out so far across
 * calls and starts zeroed.
 *
 * Shared deliberately. A caller that cannot block reads the same bytes in a
 * different order, and a second copy of response framing would be a second
 * place for chunked, Content-Length and the no-body statuses to disagree.
 */
typedef struct {
    size_t header_bytes;
    size_t body_target;
    int    chunked;
    int    definite;
    int    invalid;    /* the response did not say where its body ends */
} HttpRespFraming;

static int http_response_is_complete(HttpRespFraming* f, char* buf, size_t len,
                                     const char* method) {
    if (!f->header_bytes) {
        /* The terminator is NUL-free ASCII and precedes any body byte. */
        char* hend = strstr(buf, "\r\n\r\n");
        if (!hend) return 0;
        f->header_bytes = (size_t)((hend + 4) - buf);
        char saved = *hend;
        *hend = '\0';
        char* te = http_extract_response_header(buf, "Transfer-Encoding");
        if (te && http_value_has_chunked(te)) {
            f->chunked = 1;
            f->definite = 1;
        } else {
            /* Anchored, and counted: a response naming two different lengths
             * has not said where its body ends, and picking one leaves the
             * bytes of the other in a connection this client may hand to the
             * next request. */
            char cl[64];
            int differing = 0;
            int count = http_find_header_in_block(buf, hend, "Content-Length",
                                                  cl, sizeof(cl), &differing);
            if (count > 0) {
                if (differing) {
                    f->invalid = 1;
                } else {
                    char* endp = NULL;
                    long long declared = strtoll(cl, &endp, 10);
                    if (endp && *endp == '\0' && endp != cl && declared >= 0) {
                        f->body_target = f->header_bytes + (size_t)declared;
                        f->definite = 1;
                    } else {
                        f->invalid = 1;
                    }
                }
            }
        }
        free(te);
        *hend = saved;
        if (no_body_expected(response_status_of(buf), method)) {
            f->body_target = f->header_bytes;
            f->definite = 1;
            f->chunked = 0;
        }
    }
    if (f->invalid) return 1;      /* stop reading; the driver reports it */
    if (!f->definite) return 0;
    if (f->chunked)
        return http_chunked_complete(buf + f->header_bytes, len - f->header_bytes);
    return len >= f->body_target;
}

/* Serialise a request head into a fresh capability-gated buffer.
 *
 * Shared so that a driver which sends without blocking puts exactly the same
 * bytes on the wire as the blocking one. Returns NULL on allocation failure,
 * with nothing to free; on success the caller owns the buffer and frees it
 * with aether_caps_free(buf, *out_cap).
 */
typedef struct {
    const HttpClientRequest* req;
    const char* method;
    const char* path;
    const char* host;
    int         port;
    int         via_proxy;
    int         use_tls;
    const char* body;
    int         body_len;
    const char* content_type;
    int         keep_alive;
} HttpReqHead;

static char* http_build_request_head(const HttpReqHead* p,
                                     size_t* out_len, size_t* out_cap) {
    size_t hdr_cap = 1024;
    /* #461: gate the request-header build buffer through the cap. Self-
     * contained — alloc, grow, and free all live in this function with
     * hdr_cap tracking the live size, so accounting balances exactly. */
    char* hdr = (char*)aether_caps_malloc(hdr_cap);
    if (!hdr) return NULL;
    size_t hdr_len = 0;

    /* Helper: append a NUL-terminated string into hdr, growing as
     * needed. Returns 0 on success, -1 on OOM. */
    #define HDR_APPEND_STR(s) do { \
        size_t _slen = strlen(s); \
        if (hdr_len + _slen + 1 > hdr_cap) { \
            size_t _nc = hdr_cap; \
            while (_nc < hdr_len + _slen + 1) _nc *= 2; \
            char* _nh = (char*)aether_caps_realloc(hdr, hdr_cap, _nc); \
            if (!_nh) { aether_caps_free(hdr, hdr_cap); return NULL; } \
            hdr = _nh; hdr_cap = _nc; \
        } \
        memcpy(hdr + hdr_len, s, _slen); \
        hdr_len += _slen; \
        hdr[hdr_len] = '\0'; \
    } while (0)

    /* Request line. For plain HTTP through a forward proxy, use the absolute
     * form (`GET http://p->host[:p->port]/p->path HTTP/1.1`) so the proxy knows the
     * origin. Direct requests, and HTTPS-through-a-CONNECT-tunnel (which talks
     * end-to-end to the origin), use the origin form (`GET /p->path`). */
    HDR_APPEND_STR(p->method); HDR_APPEND_STR(" ");
    if (p->via_proxy && !p->use_tls) {
        char absline[1408];
        if (p->port == 80) {
            snprintf(absline, sizeof(absline), "http://%s%s", p->host, p->path);
        } else {
            snprintf(absline, sizeof(absline), "http://%s:%d%s", p->host, p->port, p->path);
        }
        HDR_APPEND_STR(absline);
    } else {
        HDR_APPEND_STR(p->path);
    }
    HDR_APPEND_STR(" HTTP/1.1\r\n");

    /* Built-in Host (overridable via set_header).
     *
     * The authority, which includes the port whenever it is not the default
     * for the scheme (RFC 9110 7.2). Sending the bare host to a server on
     * another port names a different authority than the one being addressed,
     * which a virtual-hosted server routes on. IPv6 literals would need
     * brackets here; the connect path is IPv4-only (see the note above), so
     * there is no such host to reach this. */
    if (!header_already_set(p->req, "Host")) {
        HDR_APPEND_STR("Host: "); HDR_APPEND_STR(p->host);
        if (p->port != (p->use_tls ? 443 : 80)) {
            char port_suffix[16];
            snprintf(port_suffix, sizeof(port_suffix), ":%d", p->port);
            HDR_APPEND_STR(port_suffix);
        }
        HDR_APPEND_STR("\r\n");
    }

    /* Built-in Content-Length when p->body present (overridable, but
     * setting it manually is almost always a bug — we still emit
     * ours unless the caller explicitly overrode it). */
    if (p->body && p->body_len > 0 && !header_already_set(p->req, "Content-Length")) {
        char clen[32];
        snprintf(clen, sizeof(clen), "Content-Length: %d\r\n", p->body_len);
        HDR_APPEND_STR(clen);
    }

    /* Built-in Content-Type when p->body present, only if neither the
     * builder's p->content_type nor an explicit Content-Type header is set. */
    if (p->body && p->body_len > 0 && p->content_type
        && !header_already_set(p->req, "Content-Type")) {
        HDR_APPEND_STR("Content-Type: "); HDR_APPEND_STR(p->content_type); HDR_APPEND_STR("\r\n");
    } else if (p->body && p->body_len > 0 && !p->content_type
        && !header_already_set(p->req, "Content-Type")) {
        HDR_APPEND_STR("Content-Type: application/x-www-form-urlencoded\r\n");
    }

    /* Persistent by default: the connection goes back to the idle pool when
     * the response framing is definite. A caller who sets their own
     * Connection header still gets exactly that. */
    if (!header_already_set(p->req, "Connection")) {
        HDR_APPEND_STR(p->keep_alive ? "Connection: keep-alive\r\n"
                                 : "Connection: close\r\n");
    }

    /* Caller-provided headers, in insertion order. */
    for (HttpHeader* h = p->req->headers; h; h = h->next) {
        HDR_APPEND_STR(h->name); HDR_APPEND_STR(": "); HDR_APPEND_STR(h->value); HDR_APPEND_STR("\r\n");
    }

    /* End-of-headers blank line. */
    HDR_APPEND_STR("\r\n");

    #undef HDR_APPEND_STR

    *out_len = hdr_len;
    *out_cap = hdr_cap;
    return hdr;
}

/* Fill status, headers and body from a complete response buffer.
 *
 * Shared with any driver that accumulates the same bytes without blocking, so
 * de-chunking and the header/body split cannot come out differently depending
 * on who read the socket. `buf` is modified in place: the header terminator is
 * NUL-terminated so the header block can be handed out as a string.
 */
static void http_response_fill_from_bytes(HttpResponse* response,
                                          char* buf, size_t len) {
    char* header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) {
        response->body = string_new_with_length(buf, len);
        return;
    }
    size_t header_bytes = (size_t)((header_end + 4) - buf);
    size_t body_bytes = len >= header_bytes ? len - header_bytes : 0;
    *header_end = '\0';
    char* status_line = buf;
    response->status_code = response_status_of(status_line);
    if (response->status_code == 0 && !response->error) {
        /* Headers arrived, but the first line does not carry a status, so
         * this is not a response to hand back as a success: a proxy would
         * copy the absent status straight onto its own reply. */
        response->error = string_new("malformed status line in response");
    }

    response->headers = string_new(buf);

    /* De-chunk a `Transfer-Encoding: chunked` body so consumers see
     * the decoded payload, not the raw chunk framing
     * (`13\r\n…\r\n0\r\n\r\n`). Without this, any unknown-length /
     * streamed upstream response (no Content-Length) came back
     * framed — corrupting e.g. VCR record-mode tapes
     * (vcr_record_chunked_dechunk_wish.md). Gated on the header, so
     * only chunked bodies — already garbled today — change shape;
     * on malformed framing we keep the raw bytes. */
    const char* body_start = header_end + 4;
    char* te = http_extract_response_header(buf, "Transfer-Encoding");
    char* dechunked = NULL;
    size_t dechunked_len = 0;
    if (te && http_value_has_chunked(te)) {
        dechunked = http_dechunk(body_start, body_bytes, &dechunked_len);
    }
    free(te);
    if (dechunked) {
        response->body = string_new_with_length(dechunked, dechunked_len);
        free(dechunked);
    } else {
        response->body = string_new_with_length(body_start, body_bytes);
    }
}

/* ---- The upstream exchange: write the request, read one response ----
 *
 * Everything on a client call that waits for the peer happens here, and
 * nothing else does. A driver that owns its thread loops until the exchange
 * says DONE, and never sees a WANT because a blocking transport does not
 * produce one. A driver that cannot block hands the descriptor to a poller on
 * a WANT and comes back. Both run this code, so there is one implementation
 * of what a request looks like on the wire and one of when a response has
 * finished arriving.
 *
 * The exchange does not own the transport and does not close it: whether a
 * failure is worth a redial is the driver's decision, and only the driver
 * knows whether the connection came from the pool.
 */
#define AE_X_DONE        0
#define AE_X_WANT_READ   1
#define AE_X_WANT_WRITE  2
#define AE_X_ERROR     (-1)

typedef struct {
    Transport*  t;
    const char* head;
    size_t      head_len;
    size_t      head_sent;
    const char* body;
    int         body_len;
    int         body_sent;
    const char* method;

    char*  buf;          /* accumulated response, owned by the exchange */
    size_t len;
    size_t cap;
    HttpRespFraming framing;
    int    peer_closed;  /* the peer ended the response by closing */
    int    complete;     /* the response ended where its own framing said */
    int    oom;          /* the allocator refused; nothing else went wrong */
} HttpExchange;

static void http_exchange_init(HttpExchange* x, Transport* t,
                               const char* head, size_t head_len,
                               const char* body, int body_len,
                               const char* method) {
    memset(x, 0, sizeof(*x));
    x->t = t;
    x->head = head;
    x->head_len = head_len;
    x->body = body;
    x->body_len = body_len;
    x->method = method;
}

/* Would this send/recv have blocked, rather than failed? */
static int http_io_would_block(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* Write the request head, then the body. Bodies are written raw so embedded
 * NULs survive. */
static int http_exchange_send(HttpExchange* x) {
    while (x->head_sent < x->head_len) {
        int n = transport_send(x->t, x->head + x->head_sent,
                               (int)(x->head_len - x->head_sent));
        if (n > 0) { x->head_sent += (size_t)n; continue; }
        return http_io_would_block() ? AE_X_WANT_WRITE : AE_X_ERROR;
    }
    while (x->body && x->body_sent < x->body_len) {
        int n = transport_send(x->t, x->body + x->body_sent,
                               x->body_len - x->body_sent);
        if (n > 0) { x->body_sent += n; continue; }
        return http_io_would_block() ? AE_X_WANT_WRITE : AE_X_ERROR;
    }
    return AE_X_DONE;
}

/* Read until this response ends where its own framing says it does, or until
 * the peer closes, which is the framing when nothing else declares one. */
static int http_exchange_recv(HttpExchange* x) {
    char chunk[8192];
    for (;;) {
        int n = transport_recv(x->t, chunk, sizeof(chunk) - 1);
        if (n == 0) { x->peer_closed = 1; return AE_X_DONE; }
        if (n < 0)  return http_io_would_block() ? AE_X_WANT_READ : AE_X_ERROR;

        if (x->len + (size_t)n + 1 > x->cap) {
            size_t new_cap = x->cap ? x->cap * 2 : 16384;
            while (new_cap < x->len + (size_t)n + 1) new_cap *= 2;
            /* #461: the response body is attacker-controlled (a malicious
             * server can flood it), so the doubling buffer is gated through
             * the capability allocator. `cap` carries the old size for the
             * realloc delta and for the error-path free. */
            char* grown = (char*)aether_caps_realloc(x->buf, x->cap, new_cap);
            if (!grown) { x->oom = 1; return AE_X_ERROR; }
            x->buf = grown;
            x->cap = new_cap;
        }
        memcpy(x->buf + x->len, chunk, (size_t)n);
        x->len += (size_t)n;
        x->buf[x->len] = '\0';

        if (http_response_is_complete(&x->framing, x->buf, x->len, x->method)) {
            x->complete = 1;
            return AE_X_DONE;
        }
    }
}

static HttpResponse* http_request_internal(HttpClientRequest* req) {
    const char* method = req->method;
    const char* url    = req->url;
    const char* body   = req->body;          /* may be NULL */
    int   body_len     = req->body_len;
    const char* content_type = req->content_type;
    http_init();

    HttpResponse* response = (HttpResponse*)malloc(sizeof(HttpResponse));
    if (!response) return NULL;
    response->status_code = 0;
    response->body = NULL;
    response->headers = NULL;
    response->error = NULL;
    response->redirect_error = NULL;
    response->effective_url = NULL;
    response->stream = NULL;

    char host[256];
    char path[1024];
    int port;
    int use_tls;

    if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path), &use_tls)) {
        response->error = string_new("malformed URL");
        return response;
    }

    if (use_tls) {
#ifndef AETHER_HAS_OPENSSL
        response->error = string_new("HTTPS requested but the build has no OpenSSL support (rebuild with OpenSSL installed)");
        return response;
#endif
    }

    /* Forward-proxy resolution (aether#1012). Default is direct. When a proxy
     * applies, we CONNECT to the proxy's host/port instead of the target's;
     * the original target host/port stay in `host`/`port` for the Host header,
     * the CONNECT line (https), and the absolute-form request line (http). */
    char connect_host[256];
    int  connect_port;
    int  via_proxy = 0;
    {
        char phost[256];
        int  pport = 0;
        const char* perr = NULL;
        int pr = resolve_proxy_for(req, host, use_tls,
                                   phost, sizeof(phost), &pport, &perr);
        if (pr < 0) {
            response->error = string_new(perr ? perr : "proxy error");
            return response;
        }
        if (pr == 1) {
            via_proxy = 1;
            snprintf(connect_host, sizeof(connect_host), "%s", phost);
            connect_port = pport;
        } else {
            snprintf(connect_host, sizeof(connect_host), "%s", host);
            connect_port = port;
        }
    }

    /* From here the socket connects to connect_host:connect_port (the proxy
     * when via_proxy, else the origin). Rebind the resolve/connect variables. */
    const char* dial_host = connect_host;
    int         dial_port = connect_port;

    /* The dial address, resolved lazily (#1719).
     *
     * Resolution used to run unconditionally, above the pool lookup, so every
     * request resolved the backend host and threw the answer away on a pooled
     * hit -- a lock-taking call per request for a result nobody used. Only the
     * two http_dial sites consume it, and the pool key is built from
     * dial_host/dial_port rather than the resolved address, so nothing before
     * a dial needs it.
     *
     * Both dial sites go through resolve_dial_addr, which resolves at most
     * once per request: the send-failure retry below re-dials a connection
     * that came from the pool, and that is precisely the path where the first
     * resolve was skipped.
     *
     * One deliberate behaviour change falls out of this: a request that hits a
     * live pooled connection now succeeds even if the host has since stopped
     * resolving, where before it failed with "could not resolve host". An open
     * connection does not need DNS, and holding an established socket hostage
     * to a resolver blip is the less useful behaviour -- it is also what nginx
     * does. A request that has to dial still fails exactly as it did. */
    struct sockaddr_in serv_addr;
    int serv_addr_resolved = 0;

    /* Zero-initialised so applied_timeout_ns starts at the "nothing applied"
     * sentinel rather than stack garbage, which the reuse guard would read as
     * a real value and wrongly skip the setsockopt. */
    Transport t = {0};
    t.applied_timeout_ns = -1;
    char pool_key[HTTP_POOL_KEY_MAX];
    http_pool_key(pool_key, sizeof(pool_key), host, port, use_tls,
                  dial_host, dial_port, req->insecure, req->cafile);
    /* A streaming response hands the transport to the caller, who may abandon
     * it mid-body, so those connections are never pooled in either direction.
     * A caller who set their own Connection header gets exactly that, and a
     * connection the peer is about to close is not worth keeping. */
    int pool_this = http_pool_enabled && !req->stream &&
                    !header_already_set(req, "Connection");
    int reused = 0;
    if (pool_this && http_pool_take(pool_key, &t)) {
        /* Taken without probing it first (#1719).
         *
         * This used to poll the socket before reusing it, one syscall on
         * every request through the pool, to catch a peer that had closed
         * during the idle window. The read path already handles that case and
         * handles it better: nothing coming back on a pooled connection means
         * the request went into the void, and it redials and resends. The
         * poll only moved the discovery earlier at the cost of asking the
         * kernel every time.
         *
         * The other thing it caught — bytes waiting on a supposedly idle
         * connection, which would be read as the head of this response — is
         * caught below instead, by checking that what came back actually
         * starts a response. That costs nothing and is a stronger check: the
         * poll only knew that something was readable, not what it was. */
        reused = 1;
        transport_apply_timeouts(&t, req->timeout_ns);
    }
    if (!reused) {
        if (!resolve_dial_addr(dial_host, dial_port, &serv_addr,
                               &serv_addr_resolved)) {
            response->error = string_new(via_proxy ? "could not resolve proxy host"
                                                   : "could not resolve host");
            return response;
        }

        char* dial_err = NULL;
        if (http_dial(req, &serv_addr, host, port, use_tls, via_proxy, &t, &dial_err) != 0) {
            response->error = string_new(dial_err ? dial_err : "connection failed");
            free(dial_err);
            return response;
        }
    }

    /* Build the header block in a heap-allocated growing buffer so
     * we're not bounded by a 4K stack array. Body goes out as a
     * separate transport_send so binary payloads with embedded NULs
     * survive (the previous "%s" snprintf path would have truncated
     * at the first NUL — wasn't a problem in practice because the
     * v1 wrappers only sent textual JSON, but v2 takes body+len). */
    size_t hdr_cap = 0, hdr_len = 0;
    HttpReqHead head_params = {
        req, method, path, host, port, via_proxy, use_tls,
        body, body_len, content_type, pool_this
    };
    char* hdr = http_build_request_head(&head_params, &hdr_len, &hdr_cap);
    if (!hdr) {
        transport_close(&t);
        response->error = string_new("out of memory building request");
        return response;
    }

    /* Accumulator for the buffered read below, declared here because the
     * reuse retry rewinds to `send_request` and has to reset it. */
    char*  full_response = NULL;
    size_t total_len = 0;
    size_t cap = 0;
    int    n = 0;
    int    recv_err = 0;
    int    truncated = 0;
    HttpRespFraming framing = {0};
    /* A pooled connection the peer closed while it sat idle is
     * indistinguishable from a live one until it is used: the liveness probe
     * catches almost all of them, and the rest fail with nothing received.
     * Measured against an upstream that closes after every response, 4% of
     * requests landed in that window. One redial covers it. The retry is
     * bounded to a connection that came from the pool and produced no
     * response byte, so the server cannot have acted on the request. */
    int retried = 0;
    HttpExchange tx;

send_request:
    http_exchange_init(&tx, &t, hdr, hdr_len, body, body_len, method);
    /* A WANT here is SO_SNDTIMEO firing on a transport this driver owns
     * outright, which is a failed write like any other. */
    if (http_exchange_send(&tx) != AE_X_DONE) {
        if (reused && !retried) {
            retried = 1;
            reused = 0;
            transport_close(&t);
            char* rd_err = NULL;
            /* This connection came from the pool, so the resolve above was
             * skipped; do it now. A failure here just means no retry. */
            if (resolve_dial_addr(dial_host, dial_port, &serv_addr,
                                  &serv_addr_resolved)
                && http_dial(req, &serv_addr, host, port, use_tls, via_proxy, &t, &rd_err) == 0) {
                free(rd_err);
                goto send_request;
            }
            free(rd_err);
        }
        aether_caps_free(hdr, hdr_cap);
        transport_close(&t);
        response->error = string_new("send failed");
        return response;
    }

    /* Streaming mode (#1004): read only the header block, then hand the
     * still-open transport to an HttpStream so the caller pulls the body
     * incrementally (peak memory = one read window, not O(Content-Length)).
     * The buffered read-until-EOF path below is skipped. */
    if (req->stream) {
        /* A stream is never taken from the pool, so it never retries and the
         * request buffer is done with here. */
        aether_caps_free(hdr, hdr_cap);
        hdr = NULL;
        char   sbuf[8192];
        char*  hb = NULL;        /* headers + any over-read body bytes */
        size_t hlen = 0, hcap = 0;
        char*  hend = NULL;
        int    sn, serr = 0;
        while ((sn = transport_recv(&t, sbuf, sizeof(sbuf))) > 0) {
            if (hlen + (size_t)sn + 1 > hcap) {
                size_t nc = hcap ? hcap * 2 : 16384;
                while (nc < hlen + (size_t)sn + 1) nc *= 2;
                char* nb = (char*)realloc(hb, nc);
                if (!nb) {
                    free(hb);
                    transport_close(&t);
                    response->error = string_new("out of memory reading response headers");
                    return response;
                }
                hb = nb; hcap = nc;
            }
            memcpy(hb + hlen, sbuf, (size_t)sn);
            hlen += (size_t)sn;
            hb[hlen] = '\0';
            /* The header terminator is NUL-free ASCII and precedes any body
             * byte, so strstr finds it before any body NUL. */
            hend = strstr(hb, "\r\n\r\n");
            if (hend) break;
        }
        if (sn < 0) serr = 1;
        if (!hend) {
            free(hb);
            transport_close(&t);
            response->error = string_new(
                serr ? "recv timeout or I/O error"
                     : "connection closed before response headers");
            return response;
        }

        size_t header_bytes = (size_t)((hend + 4) - hb);
        size_t over_len     = hlen - header_bytes;   /* body bytes already read */
        *hend = '\0';                                 /* isolate the header block */

        response->status_code = response_status_of(hb);
        response->headers = string_new(hb);

        /* Framing: chunked wins over Content-Length; neither => read-until-close. */
        int       is_chunked = 0;
        long long clen = -1;
        char* te = http_extract_response_header(hb, "Transfer-Encoding");
        if (te && http_value_has_chunked(te)) is_chunked = 1;
        free(te);
        if (!is_chunked) {
            char* cl = http_extract_response_header(hb, "Content-Length");
            if (cl) { clen = strtoll(cl, NULL, 10); free(cl); }
        }

        struct HttpStream* st = (struct HttpStream*)calloc(1, sizeof(struct HttpStream));
        if (!st) {
            free(hb);
            transport_close(&t);
            response->error = string_new("out of memory");
            return response;
        }
        st->t = t;                 /* transfer ownership of the open transport */
        st->chunked = is_chunked;
        if (is_chunked) {
            st->chunk_state = 0;   /* start by reading a chunk-size line */
        } else if (clen >= 0) {
            st->content_remaining = clen;
            if (clen == 0) st->eof = 1;   /* declared empty body */
        } else {
            st->read_until_close = 1;
        }
        if (over_len > 0) {
            st->pending = (char*)malloc(over_len);
            if (!st->pending) {
                free(hb);
                http_stream_free(st);   /* closes the transport it now owns */
                response->error = string_new("out of memory");
                return response;
            }
            memcpy(st->pending, hend + 4, over_len);
            st->pending_len = (int)over_len;
            st->pending_cap = (int)over_len;
        }
        free(hb);
        response->stream = st;
        return response;           /* transport stays open, owned by the stream */
    }

    /* Read to the end of THIS response, not to end of connection: the header
     * block, then exactly the body its framing declares. Reading to EOF works
     * only when the connection is the delimiter, which is what made every
     * request cost a fresh connection (#1653). `body_target` is the total
     * byte count that ends the response, or 0 while it is still unknown.
     * The accumulator grows by doubling; a realloc per recv was quadratic on
     * large responses. */
    {
        HttpExchange rx;
        http_exchange_init(&rx, &t, NULL, 0, NULL, 0, method);
        int rc = http_exchange_recv(&rx);
        full_response = rx.buf;
        total_len     = rx.len;
        cap           = rx.cap;
        framing       = rx.framing;
        /* A response that never said where its body ends is not one to hand
         * back: the bytes it did not account for belong to nothing, and on a
         * pooled connection they would be read as the head of the next
         * response. */
        if (rx.framing.invalid) {
            aether_caps_free(full_response, cap);
            transport_close(&t);
            response->error = string_new(
                "response framing is ambiguous: the declared body length is not usable");
            return response;
        }
        if (rx.oom) {
            aether_caps_free(full_response, cap);
            transport_close(&t);
            response->error = string_new("out of memory reading response");
            return response;
        }
        /* A WANT on a blocking transport is SO_RCVTIMEO firing, not a socket
         * with more to say later: this driver owns its thread and never asked
         * for a non-blocking one. Treating it as anything but a failed read
         * is what once let a timed-out request return an empty 0-status
         * response that the caller could not tell from a silent server. */
        if (rc == AE_X_ERROR || rc == AE_X_WANT_READ || rc == AE_X_WANT_WRITE)
            recv_err = 1;
        /* The response declared its own length and stopped short of it. The
         * body we hold is a prefix of the real one, and handing that back as
         * a successful response makes a truncated payload indistinguishable
         * from a complete one: a proxy forwards a short body as if it were
         * whole, and a caller parses whatever arrived. A response with no
         * declared framing is not this case, because there the close IS the
         * framing. */
        truncated = rx.framing.definite && !rx.complete;
        n = 0;
    }
    /* Nothing at all came back on a connection that came from the pool: the
     * peer had closed it and our request went into the void. Redial once and
     * send again; the server never saw this request, so repeating it is safe
     * whatever the method. The desync case below is the same reasoning: what
     * came back was not an answer to this request. */
    /* A pooled connection that answers with something that is not a response
     * was carrying bytes nobody asked for, and reading them as this
     * response's head would desynchronise everything after it. Same treatment
     * as no answer at all: retire the connection and ask again on a fresh
     * one. Only for a connection that came from the pool — a freshly dialled
     * one returning nonsense is the upstream's answer, not a stale socket. */
    int desynced = reused && total_len > 0
                && (total_len < 5 || strncmp(full_response, "HTTP/", 5) != 0);

    if ((total_len == 0 || desynced) && reused && !retried) {
        retried = 1;
        reused = 0;
        transport_close(&t);
        char* rd_err = NULL;
        /* Pooled connection, so the resolve was skipped; do it now. */
        if (resolve_dial_addr(dial_host, dial_port, &serv_addr,
                              &serv_addr_resolved)
            && http_dial(req, &serv_addr, host, port, use_tls, via_proxy, &t, &rd_err) == 0) {
            free(rd_err);
            aether_caps_free(full_response, cap);
            full_response = NULL;
            total_len = 0;
            cap = 0;
            recv_err = 0;
            framing.header_bytes = 0;
            framing.body_target = 0;
            framing.chunked = 0;
            framing.definite = 0;
            goto send_request;
        }
        /* The redial failed too, so the upstream is gone rather than the
         * pooled connection being stale. Say that instead of handing back an
         * empty status-0 response. */
        aether_caps_free(hdr, hdr_cap);
        aether_caps_free(full_response, cap);
        response->error = string_new(rd_err ? rd_err : "connection failed");
        free(rd_err);
        return response;
    }

    aether_caps_free(hdr, hdr_cap);
    hdr = NULL;

    /* n < 0 means transport_recv hit an error; the most common one
     * (when the caller set a timeout) is recv-side EAGAIN/EWOULDBLOCK
     * from SO_RCVTIMEO firing. Without this, a timed-out request
     * silently returned an empty 0-status response — caller couldn't
     * tell timeout from a happy server returning nothing. */
    if (n < 0) {
        recv_err = 1;
    }

    // Zero-byte response: still need a valid empty string so strstr
    // below is safe. Tiny allocation, done only on the empty path.
    if (!full_response) {
        full_response = (char*)aether_caps_malloc(1);
        if (!full_response) {
            transport_close(&t);
            response->error = string_new("out of memory");
            return response;
        }
        full_response[0] = '\0';
        cap = 1;  /* #461: record the 1-byte size so the frees below balance */
    }

    /* Hand the connection back only when this response ended where its own
     * framing said it would, and neither side asked to close. Anything else
     * (read-until-EOF framing, a truncated body, an I/O error) leaves the
     * next response's start position unknown, so the connection is retired. */
    {
        int keep = pool_this && !recv_err && framing.definite &&
                   (framing.chunked || total_len == framing.body_target);
        if (keep && framing.header_bytes > 0) {
            char saved = full_response[framing.header_bytes - 4];
            full_response[framing.header_bytes - 4] = '\0';
            char* conn_hdr = http_extract_response_header(full_response, "Connection");
            if (conn_hdr) {
                if (http_strcasestr_local(conn_hdr, "close")) keep = 0;
                free(conn_hdr);
            } else if (strncmp(full_response, "HTTP/1.0", 8) == 0) {
                keep = 0;   /* HTTP/1.0 closes unless it says otherwise */
            }
            full_response[framing.header_bytes - 4] = saved;
        } else {
            keep = 0;
        }
        if (keep) http_pool_put(pool_key, &t);
        else      transport_close(&t);
    }

    /* If transport_recv reported an error AND we didn't get a complete
     * header block, this was a timeout / I/O error mid-recv. Tell the
     * caller — otherwise they'd see status=0 + empty body and not
     * know whether the request even reached the server. */
    char* header_end = strstr(full_response, "\r\n\r\n");
    if (recv_err && !header_end) {
        aether_caps_free(full_response, cap);
        response->error = string_new("recv timeout or I/O error");
        return response;
    }
    /* A response that stopped short of the length it declared is an error
     * even though its headers arrived intact. Reporting only the no-headers
     * case meant a read that died mid-body came back as a successful short
     * response, which a caller cannot tell from a complete one. */
    if (truncated) {
        aether_caps_free(full_response, cap);
        response->error = string_new(
            recv_err ? "response truncated: read failed before the declared body length"
                     : "response truncated: peer closed before the declared body length");
        return response;
    }
    /* Hand over exactly the body the response declared. A read can return
     * more than that (a pipelined reply, or bytes a mis-framed response left
     * behind), and counting those as body makes the payload something the
     * sender never described. Chunked framing carries its own end, so it is
     * left alone. */
    size_t deliver_len = total_len;
    if (framing.definite && !framing.chunked && framing.body_target > 0
        && framing.body_target < deliver_len)
        deliver_len = framing.body_target;
    http_response_fill_from_bytes(response, full_response, deliver_len);

    aether_caps_free(full_response, cap);
    return response;
}

/* Case-insensitive header-name match. Used both when emitting the
 * built-in headers (skip if caller already set one) and when looking
 * up a response header by name. */
static int http_strcaseeq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int header_already_set(const HttpClientRequest* req, const char* name) {
    if (!req) return 0;
    for (HttpHeader* h = req->headers; h; h = h->next) {
        if (http_strcaseeq(h->name, name)) return 1;
    }
    return 0;
}

/* Look up the case-folded value of a response header from the raw
 * header block. Lighter than http_response_header_raw because it
 * works directly on the string buffer rather than the response
 * struct (so the redirect loop below can use it without releasing
 * intermediate responses). Returns a freshly-allocated copy the
 * caller must free, or NULL if the header isn't present. */
static char* http_extract_response_header(const char* hdr_block, const char* name) {
    if (!hdr_block || !name) return NULL;
    size_t name_len = strlen(name);
    const char* p = hdr_block;
    /* Skip the status line. */
    const char* nl = strchr(p, '\n');
    if (nl) p = nl + 1;
    while (*p) {
        const char* line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        const char* colon = memchr(p, ':', line_len);
        if (colon && (size_t)(colon - p) == name_len) {
            int match = 1;
            for (size_t i = 0; i < name_len; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { match = 0; break; }
            }
            if (match) {
                const char* v = colon + 1;
                while (v < p + line_len && (*v == ' ' || *v == '\t')) v++;
                size_t vlen = (size_t)((p + line_len) - v);
                while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ' ||
                                    v[vlen - 1] == '\t')) vlen--;
                char* out = (char*)malloc(vlen + 1);
                if (!out) return NULL;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return out;
            }
        }
        if (!line_end) break;
        p = line_end + 1;
    }
    return NULL;
}

/* Case-insensitive: does the Transfer-Encoding value name `chunked`?
 * `chunked` is the final (and in practice only) transfer coding we
 * decode; a comma-list like "gzip, chunked" still matches. */
static int http_value_has_chunked(const char* v) {
    if (!v) return 0;
    for (const char* p = v; *p; p++) {
        if ((*p == 'c' || *p == 'C') &&
            (p[1] == 'h' || p[1] == 'H') && (p[2] == 'u' || p[2] == 'U') &&
            (p[3] == 'n' || p[3] == 'N') && (p[4] == 'k' || p[4] == 'K') &&
            (p[5] == 'e' || p[5] == 'E') && (p[6] == 'd' || p[6] == 'D')) {
            return 1;
        }
    }
    return 0;
}

/* Decode HTTP/1.1 chunked transfer-encoding (RFC 7230 §4.1). `in` /
 * `in_len` is the raw chunk-framed body; writes the decoded length to
 * *out_len and returns a malloc'd buffer (NUL-terminated for
 * convenience; *out_len excludes the NUL). Returns NULL on malformed
 * framing so the caller can fall back to the raw bytes. Binary-safe:
 * the payload is copied by length, never scanned for NUL. Chunk
 * extensions (`<size>;name=val`) are skipped; trailing trailers after
 * the terminating `0` chunk are ignored. */
char* http_dechunk(const char* in, size_t in_len, size_t* out_len) {
    if (!in || !out_len) return NULL;
    char* out = (char*)malloc(in_len + 1);   /* decoded payload <= input */
    if (!out) return NULL;
    size_t oi = 0;
    size_t i = 0;
    while (i < in_len) {
        size_t sz = 0;
        int saw_hex = 0;
        while (i < in_len) {
            char c = in[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16u + (size_t)d;
            saw_hex = 1;
            i++;
        }
        if (!saw_hex) { free(out); return NULL; }   /* expected a hex size */
        /* Skip the rest of the size line (chunk extensions) to the LF. */
        while (i < in_len && in[i] != '\n') i++;
        if (i >= in_len) { free(out); return NULL; } /* no CRLF after size */
        i++;                                          /* consume the '\n' */
        if (sz == 0) break;                           /* terminating chunk */
        if (sz > in_len - i) { free(out); return NULL; } /* truncated chunk */
        memcpy(out + oi, in + i, sz);
        oi += sz;
        i += sz;
        /* Consume the CRLF (or bare LF) that follows the chunk data. */
        if (i < in_len && in[i] == '\r') i++;
        if (i < in_len && in[i] == '\n') i++;
    }
    out[oi] = '\0';
    *out_len = oi;
    return out;
}

/* Resolve a Location-header value against a base URL. The Location
 * may be absolute (`http://other.host/x`), scheme-relative
 * (`//other.host/x`), root-relative (`/x`), or relative
 * (`x`). Returns a malloc'd absolute URL on success, NULL on
 * malformed input. */
static char* http_resolve_location(const char* base_url, const char* location) {
    if (!base_url || !location || !*location) return NULL;
    /* Absolute URL — Location starts with a scheme. */
    if (strstr(location, "://")) return strdup(location);
    /* Need to extract scheme + host[:port] from base_url.
     * parse_url returns 1 on success, 0 on failure — bail when it
     * fails. The earlier round-1 implementation had the check
     * inverted, which produced a bogus "malformed Location header"
     * for every well-formed base URL. Caught by the cases-8-10
     * end-to-end test in test_http_client_v2.ae. */
    char base_host[256], base_path[1024];
    int base_port = 0, base_use_tls = 0;
    if (parse_url(base_url, base_host, sizeof(base_host),
                  &base_port, base_path, sizeof(base_path), &base_use_tls) == 0) {
        return NULL;
    }
    const char* scheme = base_use_tls ? "https" : "http";
    /* Scheme-relative: //host/x → keep base scheme. */
    if (location[0] == '/' && location[1] == '/') {
        size_t need = strlen(scheme) + 1 + strlen(location) + 1;
        char* out = (char*)malloc(need);
        if (!out) return NULL;
        snprintf(out, need, "%s:%s", scheme, location);
        return out;
    }
    /* Root-relative: /x → keep base scheme + host[:port]. */
    if (location[0] == '/') {
        size_t need = strlen(scheme) + 3 + strlen(base_host) + 16 + strlen(location) + 1;
        char* out = (char*)malloc(need);
        if (!out) return NULL;
        if ((base_use_tls && base_port == 443) || (!base_use_tls && base_port == 80)) {
            snprintf(out, need, "%s://%s%s", scheme, base_host, location);
        } else {
            snprintf(out, need, "%s://%s:%d%s", scheme, base_host, base_port, location);
        }
        return out;
    }
    /* Relative path: replace last segment of base_path. */
    char joined_path[1024];
    char* last_slash = strrchr(base_path, '/');
    if (last_slash) {
        size_t prefix_len = (size_t)(last_slash - base_path) + 1;
        if (prefix_len + strlen(location) + 1 > sizeof(joined_path)) return NULL;
        memcpy(joined_path, base_path, prefix_len);
        strcpy(joined_path + prefix_len, location);
    } else {
        snprintf(joined_path, sizeof(joined_path), "/%s", location);
    }
    size_t need = strlen(scheme) + 3 + strlen(base_host) + 16 + strlen(joined_path) + 1;
    char* out = (char*)malloc(need);
    if (!out) return NULL;
    if ((base_use_tls && base_port == 443) || (!base_use_tls && base_port == 80)) {
        snprintf(out, need, "%s://%s%s", scheme, base_host, joined_path);
    } else {
        snprintf(out, need, "%s://%s:%d%s", scheme, base_host, base_port, joined_path);
    }
    return out;
}

/* Strip headers that should not be forwarded across a host change
 * (Authorization, Cookie, Proxy-Authorization). Modifies req in
 * place. Called on each redirect hop where the target host differs
 * from the previous host. */
static void http_strip_cross_host_headers(HttpClientRequest* req) {
    if (!req) return;
    HttpHeader** link = &req->headers;
    while (*link) {
        HttpHeader* h = *link;
        int strip = http_strcaseeq(h->name, "Authorization") ||
                    http_strcaseeq(h->name, "Cookie") ||
                    http_strcaseeq(h->name, "Proxy-Authorization");
        if (strip) {
            *link = h->next;
            free(h);      /* name and value live in this same block */
        } else {
            link = &h->next;
        }
    }
    /* Any of those unlinked nodes could have been the tail. Recomputed
     * once here rather than tracked through the loop: this runs only on a
     * redirect whose host differs, while the insert path it keeps O(1) runs
     * for every header of every request. */
    req->headers_tail = NULL;
    for (HttpHeader* t = req->headers; t; t = t->next) req->headers_tail = t;
}

/* v2 entry point. The v1 wrappers below build a throwaway request
 * and call this. Handles redirect-following when the request was
 * configured with max_redirects > 0 (issue #239). */
HttpResponse* http_send_raw(HttpClientRequest* req) {
    if (!req) return NULL;

    HttpResponse* resp = http_request_internal(req);
    if (!resp) return NULL;

    /* Stash the URL of the originating request as the effective URL.
     * Overwritten below if redirects are followed. */
    if (req->url) resp->effective_url = string_new(req->url);

    /* If redirects aren't enabled, return the first response as-is. */
    if (req->max_redirects <= 0) return resp;

    /* No URL to resolve redirects against (strdup(NULL) below would be UB),
     * so there is nothing to follow: return the first response as-is. */
    if (!req->url) return resp;

    /* Track visited URLs for loop detection. Bounded by max_redirects
     * + 1 (the original URL). Static array is fine — max_redirects is
     * expected to be small (typically 5-10). */
    char* visited[64];
    int visited_count = 0;
    int max_track = req->max_redirects + 1;
    if (max_track > 64) max_track = 64;
    visited[visited_count++] = strdup(req->url);

    int hops_remaining = req->max_redirects;
    char* current_url = strdup(req->url);
    if (!current_url || !visited[0]) {
        // Can't follow redirects without a copy of the starting URL; return the
        // first response as-is rather than dereferencing a NULL current_url below.
        free(current_url);
        free(visited[0]);
        return resp;
    }

    while (hops_remaining > 0 && resp && resp->status_code >= 300 && resp->status_code < 400) {
        const char* hdrs = resp->headers ? string_to_cstr(resp->headers) : "";
        char* location = http_extract_response_header(hdrs, "Location");
        if (!location) break;  /* 3xx with no Location → return as-is. */

        char* next_url = http_resolve_location(current_url, location);
        free(location);
        if (!next_url) {
            /* Redirect-class error — record on redirect_error so the
             * v2 send_request wrapper preserves the response and the
             * caller can still inspect the terminal 3xx. Issue #239. */
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new("malformed Location header");
            break;
        }

        /* Reject scheme downgrade: HTTPS origin → HTTP target. */
        int curr_https = strncmp(current_url, "https://", 8) == 0;
        int next_https = strncmp(next_url, "https://", 8) == 0;
        if (curr_https && !next_https) {
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new(
                "redirect rejected: scheme downgrade (https -> http)");
            free(next_url);
            break;
        }

        /* Loop detection: refuse to revisit a URL within this chain. */
        int looped = 0;
        for (int i = 0; i < visited_count; i++) {
            if (visited[i] && strcmp(visited[i], next_url) == 0) {
                looped = 1;
                break;
            }
        }
        if (looped) {
            if (resp->redirect_error) string_release(resp->redirect_error);
            resp->redirect_error = string_new(
                "redirect loop detected (hop limit may have been exceeded)");
            free(next_url);
            break;
        }

        /* Strip cross-host auth headers if the host changed.
         * parse_url returns 1 on success — same inverted-check bug
         * as http_resolve_location had above; without this fix the
         * strip path never fired in practice (because parse_url
         * always succeeds for well-formed URLs). */
        char curr_host[256], next_host[256], dummy_path[1024];
        int curr_port = 0, next_port = 0, curr_tls = 0, next_tls = 0;
        if (parse_url(current_url, curr_host, sizeof(curr_host), &curr_port,
                      dummy_path, sizeof(dummy_path), &curr_tls) != 0 &&
            parse_url(next_url, next_host, sizeof(next_host), &next_port,
                      dummy_path, sizeof(dummy_path), &next_tls) != 0) {
            if (strcmp(curr_host, next_host) != 0) {
                http_strip_cross_host_headers(req);
            }
        }

        /* Move on. Record the new URL, swap the request URL, send,
         * release the prior response. */
        if (visited_count < max_track) {
            visited[visited_count++] = strdup(next_url);
        }
        free(current_url);
        current_url = strdup(next_url);

        free(req->url);
        req->url = next_url;  /* takes ownership */

        http_response_free(resp);
        resp = http_request_internal(req);
        if (!resp) break;

        hops_remaining--;
    }

    /* If we exited the loop because we ran out of hops while still
     * looking at a 3xx response, surface that as a redirect_error so
     * the caller can inspect the terminal 3xx status / body without
     * the v2 wrapper auto-freeing the response. */
    if (resp && hops_remaining == 0 && resp->status_code >= 300 && resp->status_code < 400) {
        if (resp->redirect_error) string_release(resp->redirect_error);
        resp->redirect_error = string_new("redirect hop limit reached");
    }

    /* Stash the final URL as the effective URL on the response. */
    if (resp) {
        if (resp->effective_url) string_release(resp->effective_url);
        resp->effective_url = string_new(current_url);
    }

    free(current_url);
    for (int i = 0; i < visited_count; i++) free(visited[i]);

    return resp;
}

/* v1 wrappers — thin sugar over the v2 builder. timeout=0 preserves
 * the original "block forever" behaviour callers had before v2. */

HttpResponse* http_get_raw(const char* url) {
    HttpClientRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

// http_get_raw + per-call timeout. Closes the polling-loop pain
// where one hung site held the actor's whole AnalyzeNext handler
// hostage. Default `http_get_raw` blocks forever for backward
// compatibility; new callers should reach for this variant any time
// the URL is third-party or non-local.
HttpResponse* http_get_with_timeout_raw(const char* url, int timeout_ms) {
    HttpClientRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    if (timeout_ms > 0) {
        // SO_RCVTIMEO / SO_SNDTIMEO storage is integer seconds; round
        // up so callers asking for 50ms get a 1s timeout rather than
        // accidentally disabling the timeout (0 = block forever). The
        // sub-second granularity gap is documented; ms-precision
        // timeouts are a separate change to the underlying field.
        int secs = (timeout_ms + 999) / 1000;
        http_request_set_timeout_raw(req, secs);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_get_with_timeout_ns_raw(const char* url, int64_t timeout_ns) {
    HttpClientRequest* req = http_request_raw("GET", url);
    if (!req) return NULL;
    if (http_request_set_timeout_ns_raw(req, timeout_ns) != 0) {
        http_request_free_raw(req);
        return NULL;
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_post_raw(const char* url, const char* body, const char* content_type) {
    HttpClientRequest* req = http_request_raw("POST", url);
    if (!req) return NULL;
    if (body) {
        http_request_set_body_raw(req, body, (int)strlen(body), content_type);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_put_raw(const char* url, const char* body, const char* content_type) {
    HttpClientRequest* req = http_request_raw("PUT", url);
    if (!req) return NULL;
    if (body) {
        http_request_set_body_raw(req, body, (int)strlen(body), content_type);
    }
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

HttpResponse* http_delete_raw(const char* url) {
    HttpClientRequest* req = http_request_raw("DELETE", url);
    if (!req) return NULL;
    HttpResponse* resp = http_send_raw(req);
    http_request_free_raw(req);
    return resp;
}

void http_response_free(HttpResponse* response) {
    if (!response) return;
    if (response->body) string_release(response->body);
    if (response->headers) string_release(response->headers);
    if (response->error) string_release(response->error);
    if (response->redirect_error) string_release(response->redirect_error);
    if (response->effective_url) string_release(response->effective_url);
    /* #1004: a streaming response owns its still-open transport; freeing the
     * response closes the socket/SSL and releases the pending buffer. This
     * also makes redirect-following safe for streaming requests: the redirect
     * loop frees each intermediate response before the next hop. */
    if (response->stream) http_stream_free(response->stream);
    free(response);
}

// Response accessors. All NULL-safe: callers can pass a NULL response
// (e.g. from an out-of-memory path) without crashing.

int http_response_status(HttpResponse* response) {
    if (!response) return 0;
    return response->status_code;
}

/* Returns an OWNED string: the response's body AetherString, retained so it
 * outlives http_response_free. Declared `@heap` on the Aether side, so codegen
 * releases the caller's ref at scope exit (aether_heap_str_free dispatches on
 * the magic header → string_release). This is what makes reading the body
 * AFTER response_free safe — a borrowed `->data` pointer would dangle, the
 * classic footgun in http-serve-and-dial-reentrancy-ask.md. On the empty/NULL
 * path we hand back a fresh empty AetherString rather than a literal "", so the
 * caller's uniform `@heap` release is always well-typed. */
const char* http_response_body(HttpResponse* response) {
    if (!response || !response->body) return (const char*)string_empty();
    string_retain(response->body);
    return (const char*)response->body;
}

int http_response_body_length(HttpResponse* response) {
    if (!response || !response->body) return 0;
    return (int)aether_string_length(response->body);
}

/* #1004: 1 if this response is streaming (body pulled incrementally via
 * http_response_read_chunk_raw), 0 if the body was buffered. */
int http_response_is_stream_raw(HttpResponse* response) {
    return (response && response->stream) ? 1 : 0;
}

/* #1004: pull the next decoded body window from a streaming response. Returns
 * a freshly-minted AetherString of up to `max` decoded body bytes (binary-safe
 * via its length). An EMPTY string means end-of-body OR a mid-stream transport
 * error; the two are disambiguated by http_response_error (set on error). For a
 * non-streaming or NULL response, returns an empty string. The caller owns the
 * returned string (`@heap` on the Aether side releases it at scope exit). */
const char* http_response_read_chunk_raw(HttpResponse* response, int max) {
    if (!response || !response->stream) return (const char*)string_empty();
    if (max <= 0) max = 65536;
    char* buf = (char*)malloc((size_t)max);
    if (!buf) {
        if (!response->error) response->error = string_new("out of memory");
        return (const char*)string_empty();
    }
    int n = stream_read_decoded(response->stream, buf, max);
    if (n < 0) {
        /* Mid-stream transport/framing error: surface via response->error so a
         * caller that sees an empty chunk can tell failure from clean EOF. */
        if (!response->error) response->error = string_new("stream read error");
        free(buf);
        return (const char*)string_empty();
    }
    AetherString* s = (n > 0) ? string_new_with_length(buf, (size_t)n) : string_empty();
    free(buf);
    return (const char*)s;
}

const char* http_response_headers(HttpResponse* response) {
    if (!response || !response->headers) return "";
    const char* s = string_to_cstr(response->headers);
    return s ? s : "";
}

const char* http_response_error(HttpResponse* response) {
    if (!response || !response->error) return "";
    const char* s = string_to_cstr(response->error);
    return s ? s : "";
}

int http_response_ok(HttpResponse* response) {
    if (!response) return 0;
    if (response->error) return 0;
    return response->status_code >= 200 && response->status_code < 300;
}

// Legacy accessor aliases — thin wrappers over the short names above.
int http_response_status_code(HttpResponse* response) {
    return http_response_status(response);
}

/* Borrowed-pointer variant, kept for the legacy `_str` accessor whose callers
 * copy-on-use (v1 wrappers, tinyweb). Unlike http_response_body (now `@heap`
 * owned), this returns a pointer into the response and does NOT retain, so it
 * must not be read after http_response_free. */
const char* http_response_body_str(HttpResponse* response) {
    if (!response || !response->body) return "";
    const char* s = string_to_cstr(response->body);
    return s ? s : "";
}

const char* http_response_headers_str(HttpResponse* response) {
    return http_response_headers(response);
}

/* Case-insensitive header lookup. Walks the raw header block stored
 * in response->headers (which still includes the HTTP status line as
 * the first "header"), splits each line at the first `:`, and matches
 * the name. Returns "" when the header isn't found.
 *
 * The returned pointer is into a per-response cache so it remains
 * valid until http_response_free(). Multiple values for the same
 * header are joined with ", " (RFC 7230 §3.2.2).
 *
 * Implementation: lazy single-pass. The cache is a singly-linked
 * list of (name, value) hung off the response — we don't pre-parse
 * into a hashmap because typical responses have <30 headers and the
 * lookup count per response is tiny. */
typedef struct HttpHeaderCache {
    char* name;
    char* value;
    struct HttpHeaderCache* next;
} HttpHeaderCache;

const char* http_response_header_raw(HttpResponse* response, const char* name) {
    if (!response || !name || !*name) return "";
    if (!response->headers) return "";

    /* The response struct doesn't have a parsed-headers field; we
     * cache by attaching to a per-thread arena. Simpler and adequate:
     * just walk the raw block on every call. The cost is O(n) in
     * header bytes, but typical headers are well under 4 KB and the
     * call count per response is small (callers grab the few headers
     * they care about and move on). If a profiler ever shows this in
     * the hot path, swap in a per-response cache.
     *
     * Joining duplicate-named headers into ", "-separated value is
     * done in a thread-local accumulator below. */
    static _Thread_local char tls_joined[8192];
    tls_joined[0] = '\0';
    size_t joined_len = 0;

    const char* hdr = string_to_cstr(response->headers);
    if (!hdr) return "";

    /* Skip the status line (first line — "HTTP/1.1 200 OK"). */
    const char* p = strchr(hdr, '\n');
    if (!p) return "";
    p++;

    while (*p) {
        const char* line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        /* Trim trailing \r if present. */
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;

        const char* colon = (const char*)memchr(p, ':', line_len);
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            /* Match the header name case-insensitively. */
            if (nlen == strlen(name)) {
                int eq = 1;
                for (size_t i = 0; i < nlen; i++) {
                    char ca = p[i], cb = name[i];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) { eq = 0; break; }
                }
                if (eq) {
                    /* Skip ": " then trim leading spaces. */
                    const char* val = colon + 1;
                    size_t vlen = (size_t)((p + line_len) - val);
                    while (vlen > 0 && (*val == ' ' || *val == '\t')) { val++; vlen--; }
                    /* Append to the joined accumulator. */
                    if (joined_len > 0 && joined_len + 2 < sizeof(tls_joined)) {
                        memcpy(tls_joined + joined_len, ", ", 2);
                        joined_len += 2;
                    }
                    if (joined_len + vlen < sizeof(tls_joined)) {
                        memcpy(tls_joined + joined_len, val, vlen);
                        joined_len += vlen;
                        tls_joined[joined_len] = '\0';
                    }
                }
            }
        }

        if (!line_end) break;
        p = line_end + 1;
    }

    return tls_joined;
}

const char* http_response_effective_url_raw(HttpResponse* response) {
    if (!response || !response->effective_url) return "";
    const char* s = string_to_cstr(response->effective_url);
    return s ? s : "";
}

const char* http_response_redirect_error_raw(HttpResponse* response) {
    if (!response || !response->redirect_error) return "";
    const char* s = string_to_cstr(response->redirect_error);
    return s ? s : "";
}

#endif // AETHER_HAS_NETWORKING
