#ifndef AETHER_HTTP_H
#define AETHER_HTTP_H

#include "../string/aether_string.h"
#include <stdint.h>
#include <stddef.h>

/* ASCII case-insensitive compare of exactly `n` bytes, inline.
 *
 * Same answer as strncasecmp on the names this compares, without the call: the
 * proxy runs this over a handful of short fixed names on every response, where
 * the call overhead is most of the cost. Two bytes differing only in 0x20 are
 * equal only when they are letters, which is what keeps this exact rather than
 * merely close. */
static inline int http_ci_eq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x == y) continue;
        unsigned char lx = (unsigned char)(x | 0x20);
        if (lx != (unsigned char)(y | 0x20)) return 0;
        if (lx < 'a' || lx > 'z') return 0;
    }
    return 1;
}

/* Decimal digits of `v` into `out`, no terminator, returning how many were
 * written; `out` needs room for 20. Formatting one small integer through
 * snprintf costs thousands of instructions, and the proxy's hot path formats
 * three of them per request. */
static inline size_t http_write_dec(char* out, unsigned long long v) {
    char tmp[20];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + (unsigned)(v % 10)); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* #1004: opaque streaming-body handle (defined in aether_http.c). Non-NULL on
 * a response returned by a request that opted into streaming; carries the
 * still-open transport and the incremental body decoder. */
struct HttpStream;

typedef struct {
    int status_code;
    AetherString* body;
    AetherString* headers;
    /* Transport-level failure: DNS resolution failed, TCP connect
     * refused, TLS handshake error, recv timeout, OOM, etc. When set,
     * status_code is 0 and body/headers/effective_url may be NULL.
     * Callers (and the v2 send_request wrapper) should treat a
     * non-empty error here as "the request didn't make it to a
     * useful response — discard the rest". */
    AetherString* error;
    /* Redirect-loop failure: the request DID get a usable response
     * (status_code is set to the last 3xx, body/headers populated)
     * but the chain couldn't reach a 2xx within the rules. Distinct
     * from `error` so callers that opt into automatic redirects via
     * set_follow_redirects() can still inspect the terminal 3xx
     * status / body to decide whether the chain failure is fatal.
     * Reasons the field gets populated:
     *   - "redirect hop limit reached"
     *   - "redirect loop detected (...)"
     *   - "redirect rejected: scheme downgrade (https → http)"
     *   - "malformed Location header"
     * The v2 send_request wrapper does NOT auto-free the response
     * when only `redirect_error` is set — `error` remains the
     * single signal for "no response is available". Issue #239. */
    AetherString* redirect_error;
    /* The URL that produced this response. For requests where redirects
     * were not followed (max_redirects == 0, the default), this equals
     * the URL the caller passed to http_send_raw. For requests that
     * followed redirects, this is the URL of the final hop — not the
     * original — so callers can disambiguate `client.response_url(r)`
     * vs the URL they originally passed to the builder. NULL until the
     * response is populated; readable via http_response_effective_url_raw. */
    AetherString* effective_url;
    /* #1004: non-NULL when this is a streaming response — the transport is
     * still open and the body is pulled incrementally via
     * http_response_read_chunk_raw. NULL for the default buffered path.
     * http_response_free() tears this down (closing the socket/SSL). */
    struct HttpStream* stream;
} HttpResponse;

// ---------------------------------------------------------------------------
// v1 one-liners — present from day one, kept callable for backward compat.
// Internally re-implemented as thin wrappers over the v2 builder below.
// They preserve the original behaviour of "no per-request timeout — block
// forever" by handing the v2 path a 0 timeout (the explicit "no timeout"
// sentinel).
// ---------------------------------------------------------------------------

HttpResponse* http_get_raw(const char* url);
// Same as http_get_raw but with a per-call timeout. timeout_ms is
// rounded up to whole seconds because the underlying SO_RCVTIMEO /
// SO_SNDTIMEO storage is integer seconds; pass 0 for "block forever"
// (matches http_get_raw's default).
HttpResponse* http_get_with_timeout_raw(const char* url, int timeout_ms);
HttpResponse* http_get_with_timeout_ns_raw(const char* url, int64_t timeout_ns);
HttpResponse* http_post_raw(const char* url, const char* body, const char* content_type);
HttpResponse* http_put_raw(const char* url, const char* body, const char* content_type);
HttpResponse* http_delete_raw(const char* url);
void http_response_free(HttpResponse* response);

// Response field accessors. All are NULL-safe: passing NULL or a freed
// response returns a sensible default (0 or "") rather than crashing.
// Returned const char* pointers from the `_str` / headers / error accessors
// are borrowed — owned by the response struct and valid only until
// http_response_free().
/* Idle connection pool (#1653). Reuse is on by default; a connection is kept
 * only when the response framing was definite and neither side asked to close.
 * `max_idle` 0 disables reuse and drops what is held, -1 leaves a setting
 * alone. */
/* Raise the connection pool's caps to suit a reverse proxy, whose upstream
 * connections are held for the length of a request and returned, not kept for
 * a handful of hosts. Sized from the descriptor budget; only ever raises, so
 * a deliberate configuration is preserved. */
/* A millisecond clock a driver may pin for one pass of its event loop, so the
 * several readers in a request share one counter access. Pin AFTER waiting: a
 * poll that blocks for its timeout would otherwise leave the pass looking at a
 * time from before it. A thread that never pins reads the real clock. */
void     http_clock_pin(void);
void     http_clock_unpin(void);
uint64_t http_clock_ms(void);

void http_client_pool_size_for_proxy(void);

/* The pool's current caps. */
void http_client_pool_caps_raw(int* max_idle, int* max_per_host);

const char* http_client_pool_configure_raw(int max_idle, int max_per_host,
                                           int64_t idle_ns);
void http_client_pool_clear_raw(void);
int  http_client_pool_idle_count_raw(void);

int http_response_status(HttpResponse* response);
// Returns an OWNED, retained AetherString (cast to const char*). Unlike the
// borrowed accessors, its lifetime is independent of the response: it survives
// http_response_free(), so reading the body after freeing the response is safe.
// C callers read content via aether_string_data() and must string_release() it
// (Aether callers get automatic release via the `@heap` extern annotation).
// This closes the response-body use-after-free footgun where a caller freed the
// response before reading the borrowed body (http-serve-and-dial-reentrancy-ask.md).
const char* http_response_body(HttpResponse* response);
/* Byte length of the response body — binary-safe accessor that
 * partners with `http_response_body` for callers that may receive
 * payloads with embedded NULs (gzip, protobuf, image formats).
 * Returns 0 when response or body is NULL. */
int  http_response_body_length(HttpResponse* response);

// Streaming response bodies (#1004).
// http_response_is_stream_raw: 1 if the response streams its body (the request
//   opted in via http_request_set_stream_raw); 0 if the body was buffered.
// http_response_read_chunk_raw: pull the next decoded body window (up to `max`
//   bytes; <=0 uses a default). Returns a freshly-minted, OWNED AetherString
//   (binary-safe via its length; `@heap` on the Aether side releases it). An
//   EMPTY result means end-of-body OR a mid-stream error — disambiguate with
//   http_response_error (set only on error). Chunked framing is decoded
//   transparently; the caller sees payload bytes, never chunk sizes.
int http_response_is_stream_raw(HttpResponse* response);
const char* http_response_read_chunk_raw(HttpResponse* response, int max);

const char* http_response_headers(HttpResponse* response);
const char* http_response_error(HttpResponse* response);

// Convenience: returns 1 if the request succeeded (no transport error
// AND HTTP status is in the 2xx range), 0 otherwise. Use this for the
// common "did it work?" check instead of chaining error/status calls.
int http_response_ok(HttpResponse* response);

// Legacy accessor aliases kept for callers that used the older
// `_code` / `_str` names. Prefer the short names above.
int http_response_status_code(HttpResponse* response);
const char* http_response_body_str(HttpResponse* response);
const char* http_response_headers_str(HttpResponse* response);

// ---------------------------------------------------------------------------
// v2 client — request builder, full response access.
//
// Build a request with method + URL + headers + optional body + explicit
// timeout, fire it, get back the full HttpResponse with status / body /
// raw header block, plus a typed case-insensitive header lookup. The
// caller drives status interpretation — non-2xx is no longer collapsed
// to an error; only transport-level failures (DNS, connect, TLS handshake,
// timeout) populate response->error.
//
// Lifecycle:
//   req = http_request_raw("GET", "https://example.com/api/users");
//   http_request_set_header_raw(req, "Authorization", "Bearer ...");
//   http_request_set_timeout_raw(req, 30);   // seconds; 0 = block forever
//   resp = http_send_raw(req);
//   http_request_free_raw(req);
//   /* read resp via the existing http_response_* accessors */
//   http_response_free(resp);
//
// Naming: every v2 client extern carries an `http_request_` /
// `http_send_` / `http_response_header_` prefix that doesn't collide
// with the existing http_response_* accessors above OR with the
// server-side surface in aether_http_server.c (`http_server_*`,
// `http_request_body`, etc. — those stay flat for tinyweb-compat).
// ---------------------------------------------------------------------------

/* Named HttpClientRequest, not HttpRequest: the server header publishes its
 * own incoming-request type under the latter name, and the two are different
 * structs, so a translation unit could not include both public headers (#1433).
 * The proxy already worked around it by hand-declaring these prototypes with
 * this very name, which duplicated signatures the compiler could then no longer
 * cross-check; that workaround is deleted now that the header is includable.
 * The server's response type is already HttpServerResponse for the same
 * reason, so the two surfaces only ever collided here. */
typedef struct HttpClientRequest HttpClientRequest;  /* opaque */

HttpClientRequest* http_request_raw(const char* method, const char* url);

// Returns 0 on success, non-zero on failure (NULL request, OOM,
// invalid header). Header names are stored verbatim and emitted as
// `Name: value\r\n`; built-in headers the wrapper would set itself
// (Host, Content-Length) are overridden by an explicit set_header
// with the same name. Multiple values for one name produce multiple
// `Name: value` lines (RFC 7230 §3.2.2 conformant).
int http_request_set_header_raw(HttpClientRequest* req, const char* name, const char* value);

// Set the request body. `len` is explicit so binary payloads with
// embedded NULs survive. content_type may be NULL (defaults to
// application/x-www-form-urlencoded for backward compat with v1).
// Replaces any prior body.
int http_request_set_body_raw(HttpClientRequest* req, const char* body, int len, const char* content_type);

// Per-request timeout in whole seconds (v1 surface). 0 means
// "no timeout — block forever". Negative values are an error.
// Internally multiplied to nanoseconds; prefer
// `http_request_set_timeout_ns_raw` for sub-second precision.
int http_request_set_timeout_raw(HttpClientRequest* req, int seconds);

// Per-request timeout as nanoseconds. 0 means "no timeout — block
// forever". Sub-second precision is preserved through to the socket
// layer: `select` uses tv_sec + tv_usec (microsecond resolution),
// `SO_RCVTIMEO`/`SO_SNDTIMEO` use `struct timeval` (microseconds) on
// POSIX or a DWORD millisecond count on Winsock. POSIX retains full
// μs; Winsock rounds up to the next whole millisecond so that a
// sub-ms value doesn't degrade to "infinite" via DWORD=0.
int http_request_set_timeout_ns_raw(HttpClientRequest* req, int64_t timeout_ns);

// Configure automatic redirect-following on this request. `max_hops` of
// 0 (the default) keeps the v1/v2 behaviour: redirects are returned as
// 30x to the caller, which decides what to do. `max_hops > 0` follows
// up to that many redirect responses; the loop stops when a non-3xx
// status comes back, when the hop limit is reached (returns the last
// 3xx response with an error string set), when a redirect points back
// to a URL we've already visited (loop detection), or when an HTTPS
// origin tries to redirect to HTTP (scheme downgrade rejection).
//
// Authorisation headers are not forwarded across host changes; the
// builder strips Authorization / Cookie / Proxy-Authorization when the
// redirect target's host differs from the previous host. Callers that
// need cross-host auth can re-`set_header(req, ...)` between sends.
//
// Negative values are an error.
int http_request_set_follow_redirects_raw(HttpClientRequest* req, int max_hops);

// Skip TLS peer + hostname verification for THIS request (curl -k /
// wget --no-check-certificate). `on` non-zero enables the skip; 0 (default)
// verifies. Relaxed per-SSL, never on the shared process-wide SSL_CTX, so an
// insecure request cannot downgrade verification for other requests.
int http_request_set_insecure_raw(HttpClientRequest* req, int on);

// Pin a custom CA for THIS request (#1107): verify the peer against the PEM
// bundle at `path` instead of the system store, keeping peer + hostname
// verification ON. Strictly stronger than set_insecure. Per-connection (never
// touches the shared SSL_CTX); NULL/empty clears the pin. `path` is copied.
int http_request_set_cafile_raw(HttpClientRequest* req, const char* path);

// Enable streaming response bodies for THIS request (#1004). When on (non-zero),
// http_send_raw returns a response whose body is NOT buffered: it carries an
// open transport, and the caller pulls the decoded body window-by-window via
// http_response_read_chunk_raw until an empty chunk. Peak memory is one window
// rather than O(Content-Length), for multi-megabyte downloads. The caller must
// http_response_free the response (which closes the transport) when done, even
// if it stops reading early. Redirects are still followed if enabled; only the
// final hop's body streams. Default 0 = buffer the whole body.
int http_request_set_stream_raw(HttpClientRequest* req, int on);

// Forward-proxy control (aether#1012). Default is DIRECT — std.http.client does
// NOT follow $HTTP_PROXY unless the program opts in, the hardened inverse of the
// httpoxy (CVE-2016-5385) default-follow footgun. Precedence, highest first:
// ignore > explicit > env > direct.
//
//   use_env_proxy(on):    follow $HTTP_PROXY/$HTTPS_PROXY/$NO_PROXY, WITH httpoxy
//                         (refuse CGI-injected uppercase HTTP_PROXY) + SSRF
//                         (reject loopback/link-local proxy) guards.
//   use_http_proxy(url):  pin an explicit proxy; env ignored entirely (empty
//                         url = revert to direct). No SSRF guard — it's a
//                         code-visible grant.
//   ignore_http_proxy():  force direct regardless of env / any set proxy.
int http_request_use_env_proxy_raw(HttpClientRequest* req, int on);
int http_request_use_http_proxy_raw(HttpClientRequest* req, const char* proxy_url);
int http_request_ignore_http_proxy_raw(HttpClientRequest* req);

void http_request_free_raw(HttpClientRequest* req);

// Fire the configured request. Returns an HttpResponse on success
// (caller frees with http_response_free), NULL only on out-of-memory
// failures BEFORE the request is sent. Transport failures (DNS,
// connect, TLS, timeout) return a non-NULL response with the failure
// recorded in response->error and status_code == 0.
HttpResponse* http_send_raw(HttpClientRequest* req);

// Case-insensitive response-header lookup. Returns "" when the header
// isn't present. The pointer is owned by the response and valid until
// http_response_free(). Multiple values for one header are joined
// with ", " (RFC 7230 §3.2.2 conformant).
const char* http_response_header_raw(HttpResponse* response, const char* name);

// Returns the URL of the response — the original request URL when no
// redirects were followed, or the URL of the final hop when they
// were. Useful after `http_request_set_follow_redirects_raw(req, N)`
// to discover where the chain landed without re-parsing Location
// headers from response->headers. NULL/free-safe.
const char* http_response_effective_url_raw(HttpResponse* response);

// Returns the redirect-class error (hop-limit / loop / scheme-downgrade /
// malformed-Location) for a response produced by a request that opted
// into automatic redirect-following. Returns "" when the chain
// completed normally (or the request never opted in). Distinct from
// http_response_error which signals transport-level failures only.
// Issue #239.
const char* http_response_redirect_error_raw(HttpResponse* response);


/* Is this a usable header name, and a value free of the bytes that end a
 * line? Shared by the client and the server: a CR or LF written into a head
 * verbatim turns one header into several, and a doubled one ends the head and
 * starts a whole extra message the peer will act on (CWE-93 on a request,
 * CWE-113 on a response). Both sides reject rather than repair. */
int http_header_name_ok(const char* name);
int http_header_value_ok(const char* value);


/* Chunked transfer-coding, shared by the client and the server because a
 * request body and a response body are chunked the same way and two decoders
 * would be two chances to disagree.
 *
 * http_chunked_complete: does `buf` hold a whole chunked body, terminal chunk
 * included? http_dechunk: decode one, returning a malloc'd buffer the caller
 * frees (*out_len excludes the NUL), or NULL on malformed framing. */
int    http_chunked_complete(const char* buf, size_t len);
size_t http_chunked_frame_len(const char* buf, size_t len);
char* http_dechunk(const char* in, size_t in_len, size_t* out_len);


/* Find a header by name in a header block, anchored to the start of each line.
 * Returns how many times it appears, writes the first value into `out`, and
 * sets *differing when two of them disagree. */
/* The CR LF CR LF that ends a header block, or NULL. Length-bounded rather
 * than NUL-terminated, and cheaper than strstr, which pays a two-way-algorithm
 * setup before it looks at a single byte of a four byte needle.
 *
 * Declared here beside http_find_header_in_block, not in the internal header,
 * because that one is included only on POSIX and this is not POSIX-only. */
const char* http_find_header_end(const char* buf, size_t len);

/* As http_find_header_in_block, and additionally reports the first matching
 * value as a span into `block` itself. A caller that only reads the value is
 * then not bounded by any buffer size, which matters for headers whose whole
 * contents decide framing. `out`, `differing`, `out_v` and `out_vl` are each
 * optional. */
int http_find_header_span(const char* block, const char* end,
                          const char* name, char* out, size_t out_cap,
                          int* differing,
                          const char** out_v, size_t* out_vl);

int http_find_header_in_block(const char* block, const char* end,
                              const char* name, char* out, size_t out_cap,
                              int* differing);


/* An upstream connection acquired without blocking, for a driver that runs
 * many connections on one thread. `sockfd` is available immediately; when
 * `connecting` is set the connection is not established yet and the driver
 * waits for the descriptor to become writable, then calls
 * http_upstream_connected to find out whether it succeeded. */
typedef struct HttpUpstreamConnOpaque HttpUpstreamConn;

int  http_upstream_acquire(const char* host, int port, HttpUpstreamConn* out);
/* Let this thread keep a few upstream connections of its own in front of the
 * shared pool. Only for a thread that owns its connections for their whole
 * life; see the note on the cache itself. */
void http_pool_thread_cache_enable(void);
/* As above, but `allow_pool` 0 forces a fresh connection. Use it when a pooled
 * connection has just been found closed: the pool may hold more of them, and
 * spending a retry on another corpse is how a client ends up with nothing. */
int  http_upstream_acquire_ex(const char* host, int port, int allow_pool,
                              HttpUpstreamConn* out);
/* 1 when the connect has completed, 0 when it is still in flight, -1 when it
 * failed. A caller woken by a poller must handle 0 by waiting again: a wakeup
 * does not promise this descriptor is the one that became ready. */
int  http_upstream_connected(HttpUpstreamConn* c);
void http_upstream_release(HttpUpstreamConn* c, int keep);

#endif
