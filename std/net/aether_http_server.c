#include "aether_http_server.h"
#include "aether_http.h"
#include "aether_net.h"
#include "aether_http_pool.h"
#include "aether_http_park.h"
#if !defined(_WIN32)
#include <signal.h>    /* pthread_sigmask / sigset_t for the embedded-server signal mask */
/* The portable thread shim, not raw <pthread.h> (see its header note).
 * It is real pthreads on POSIX, a CRITICAL_SECTION shim on Windows, and
 * no-op stubs where AETHER_HAS_THREADS is 0. Including <pthread.h> directly
 * broke wasm32-wasi (#1655): wasi-libc SHIPS a pthread.h even though its
 * pthread_create is a stub, so its real typedefs collided with the
 * threadless shim's in the same TU. */
#include "../../runtime/utils/aether_thread.h"
#endif
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_resource_caps.h"

/* HTTP/2 (#260 Tier 2). Pull in the wrapper header up front so
 * handle_one_request's h2c-upgrade dispatch (RFC 7540 §3.2) can
 * reach AetherH2Session + the h2c factory. The actual driver
 * (handle_h2_connection) is defined further down in this file under
 * the same #ifdef gate; forward-declare it here so the upgrade
 * path can hand the preloaded session over without ordering pain. */
#ifdef AETHER_HAS_NGHTTP2
#include "../http/server/h2/aether_h2.h"
typedef struct HttpConn HttpConn;
static void handle_h2_connection(struct HttpServer* server, HttpConn* conn,
                                 AetherH2Session* preloaded);
static int conn_buffered_is_h2_preface(HttpConn* conn);
#endif

#if !AETHER_HAS_NETWORKING
// Stubs when networking is unavailable
HttpServer* http_server_create(int p) { (void)p; return NULL; }
int http_server_bind_raw(HttpServer* s, const char* h, int p) { (void)s; (void)h; (void)p; return -1; }
int http_server_port(HttpServer* s) { (void)s; return 0; }
void http_server_set_host(HttpServer* s, const char* h) { (void)s; (void)h; }
int http_server_start_raw(HttpServer* s) { (void)s; return -1; }
int http_server_start_background_raw(HttpServer* s) { (void)s; return -1; }
void http_server_stop(HttpServer* s) { (void)s; }
void http_server_free(HttpServer* s) { (void)s; }
const char* http_server_set_tls_raw(HttpServer* s, const char* c, const char* k) {
    (void)s; (void)c; (void)k; return "TLS unavailable: networking not built in";
}
const char* http_server_set_h2_raw(HttpServer* s, int m) {
    (void)s; (void)m; return "HTTP/2 unavailable: networking not built in";
}
const char* http_server_set_h2_concurrent_dispatch_raw(HttpServer* s, int n) {
    (void)s; (void)n; return "HTTP/2 unavailable: networking not built in";
}
const char* http_server_set_keepalive_raw(HttpServer* s, int e, int m, int64_t i) {
    (void)s; (void)e; (void)m; (void)i; return "keep-alive unavailable: networking not built in";
}
void http_server_drain_connection(HttpServer* s, int fd) { (void)s; (void)fd; }
const char* http_server_shutdown_graceful_raw(HttpServer* s, int64_t t) { (void)s; (void)t; return ""; }
void http_server_set_on_start(HttpServer* s, HttpLifecycleHook h, void* u) { (void)s; (void)h; (void)u; }
void http_server_set_on_stop (HttpServer* s, HttpLifecycleHook h, void* u) { (void)s; (void)h; (void)u; }
const char* http_server_set_health_probes_raw(HttpServer* s, const char* lp, const char* rp,
                                              HttpReadyCheck rc, void* ud) {
    (void)s; (void)lp; (void)rp; (void)rc; (void)ud; return "";
}
void http_server_use_request_hook(HttpServer* s, HttpRequestHook h, void* u) {
    (void)s; (void)h; (void)u;
}
const char* http_server_set_access_log_raw(HttpServer* s, const char* f, const char* p) {
    (void)s; (void)f; (void)p; return "";
}
const char* http_server_set_metrics_raw(HttpServer* s, const char* e) { (void)s; (void)e; return ""; }
void http_server_sse(HttpServer* s, const char* p, HttpSseHandler h, void* u) {
    (void)s; (void)p; (void)h; (void)u;
}
int http_sse_send_event(HttpSseConn* c, const char* n, const char* d) {
    (void)c; (void)n; (void)d; return -1;
}
int http_sse_send_event_id(HttpSseConn* c, const char* n, const char* d, const char* i) {
    (void)c; (void)n; (void)d; (void)i; return -1;
}
void http_sse_close(HttpSseConn* c) { (void)c; }
void http_server_websocket(HttpServer* s, const char* p, HttpWsHandler h, void* u) {
    (void)s; (void)p; (void)h; (void)u;
}
int http_ws_send_text(HttpWsConn* w, const char* t) { (void)w; (void)t; return -1; }
int http_ws_send_binary(HttpWsConn* w, const void* d, int l) { (void)w; (void)d; (void)l; return -1; }
int http_ws_recv(HttpWsConn* w) { (void)w; return -1; }
const char* http_ws_message_data(HttpWsConn* w) { (void)w; return ""; }
int http_ws_message_length(HttpWsConn* w) { (void)w; return 0; }
void http_ws_close(HttpWsConn* w, int c, const char* r) { (void)w; (void)c; (void)r; }
void http_server_add_route(HttpServer* s, const char* m, const char* p, HttpHandler h, void* u) { (void)s; (void)m; (void)p; (void)h; (void)u; }
void http_server_get(HttpServer* s, const char* p, HttpHandler h, void* u) { (void)s; (void)p; (void)h; (void)u; }
void http_server_post(HttpServer* s, const char* p, HttpHandler h, void* u) { (void)s; (void)p; (void)h; (void)u; }
void http_server_put(HttpServer* s, const char* p, HttpHandler h, void* u) { (void)s; (void)p; (void)h; (void)u; }
void http_server_delete(HttpServer* s, const char* p, HttpHandler h, void* u) { (void)s; (void)p; (void)h; (void)u; }
void http_server_use_middleware(HttpServer* s, HttpMiddleware m, void* u) { (void)s; (void)m; (void)u; }
void http_server_use_response_transformer(HttpServer* s, HttpResponseTransformer x, void* u) {
    (void)s; (void)x; (void)u;
}
HttpRequest* http_parse_request(const char* r) { (void)r; return NULL; }
const char* http_get_header(HttpRequest* r, const char* k) { (void)r; (void)k; return NULL; }
const char* http_get_query_param(HttpRequest* r, const char* k) { (void)r; (void)k; return NULL; }
const char* http_get_path_param(HttpRequest* r, const char* k) { (void)r; (void)k; return NULL; }
void http_request_free(HttpRequest* r) { (void)r; }
HttpServerResponse* http_response_create() { return NULL; }
void http_response_set_status(HttpServerResponse* r, int c) { (void)r; (void)c; }
void http_response_set_header(HttpServerResponse* r, const char* k, const char* v) { (void)r; (void)k; (void)v; }
void http_response_add_header(HttpServerResponse* r, const char* k, const char* v) { (void)r; (void)k; (void)v; }
void http_response_clear_headers(HttpServerResponse* r) { (void)r; }
void http_response_set_body(HttpServerResponse* r, const char* b) { (void)r; (void)b; }
void http_response_set_body_n(HttpServerResponse* r, const char* b, int n) { (void)r; (void)b; (void)n; }
void http_response_json(HttpServerResponse* r, const char* j) { (void)r; (void)j; }
void* http_response_accept_tunnel(HttpServerResponse* r) { (void)r; return NULL; }
char* http_response_serialize(HttpServerResponse* r) { (void)r; return NULL; }
void http_server_response_free(HttpServerResponse* r) { (void)r; }
int http_route_matches(const char* p, const char* u, HttpRequest* r) { (void)p; (void)u; (void)r; return 0; }
const char* http_status_text(int c) { (void)c; return "Unknown"; }
const char* http_mime_type(const char* p) { (void)p; return "application/octet-stream"; }
void http_serve_file(HttpServerResponse* r, const char* f) { (void)r; (void)f; }
void http_serve_static(HttpRequest* r, HttpServerResponse* s, void* d) { (void)r; (void)s; (void)d; }
void http_server_set_actor_handler(HttpServer* s, void (*sf)(void*), void (*snf)(void*, void*, size_t), void* (*spf)(int, void (*)(void*), size_t), void (*rf)(void*)) { (void)s; (void)sf; (void)snf; (void)spf; (void)rf; }
const char* http_request_method(HttpRequest* r) { (void)r; return ""; }
const char* http_request_path(HttpRequest* r) { (void)r; return ""; }
const char* http_request_body(HttpRequest* r) { (void)r; return ""; }
int http_request_body_length(HttpRequest* r) { (void)r; return 0; }
int http_request_body_read_raw(HttpRequest* r, int o, int m) { (void)r; (void)o; (void)m; return 0; }
int http_request_body_complete(HttpRequest* r) { (void)r; return 0; }
const char* http_get_request_body_read(void) { return ""; }
int http_get_request_body_read_length(void) { return 0; }
void http_release_request_body_read(void) {}
const char* http_request_query(HttpRequest* r) { (void)r; return ""; }
const char* http_request_remote_addr(HttpRequest* r) { (void)r; return ""; }
int http_request_remote_port(HttpRequest* r) { (void)r; return 0; }
const char* http_request_local_addr(HttpRequest* r) { (void)r; return ""; }
int http_request_local_port(HttpRequest* r) { (void)r; return 0; }
const char* http_request_scheme(HttpRequest* r) { (void)r; return "http"; }
int http_request_is_tls(HttpRequest* r) { (void)r; return 0; }
const char* http_request_http_version(HttpRequest* r) { (void)r; return ""; }
int http_request_header_count(HttpRequest* r) { (void)r; return 0; }
const char* http_request_header_name(HttpRequest* r, int i) { (void)r; (void)i; return ""; }
const char* http_request_header_value(HttpRequest* r, int i) { (void)r; (void)i; return ""; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "../../runtime/utils/aether_thread.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <sys/stat.h>          /* MinGW: stat / struct stat for #641 Range. */
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close closesocket
    typedef int socklen_t;
    #ifndef strcasecmp
        #define strcasecmp _stricmp
    #endif
    #ifndef strdup
        #define strdup _strdup
    #endif
#else
    #include <sys/socket.h>
    #include <sys/stat.h>          /* fstat / struct stat / S_ISREG */
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netinet/tcp.h>       /* TCP_CORK / TCP_NOPUSH for sendfile coalescing */
    #include <unistd.h>
    #include <fcntl.h>
    #include <limits.h>
    #include <poll.h>
    #include <errno.h>
    /* Linux + macOS sendfile(2) live in different headers with
     * different signatures. Only one is pulled in per platform. */
    #if defined(__linux__)
        #include <sys/sendfile.h>
    #elif defined(__APPLE__)
        #include <sys/uio.h>
    #endif
    // I/O polling is handled by aether_io_poller (included via multicore_scheduler.h)
#endif

// -----------------------------------------------------------------------------
// Server-side TLS (#260 Tier 0)
// -----------------------------------------------------------------------------
// Connection-level transport abstraction so the rest of the server doesn't
// need to know whether each fd is plain or TLS-wrapped. plain_recv/plain_send
// when ssl == NULL; SSL_read/SSL_write when ssl is non-NULL. SSL_accept is
// driven once per accepted connection at the top of handle_client_connection
// before the HTTP parse begins.
//
// Built only when the project links OpenSSL. AETHER_HAS_OPENSSL is defined
// by Makefile when pkg-config finds the library — same gate std.cryptography
// and the http client (TLS) already use.
#ifdef AETHER_HAS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

/* Per-connection read buffer. Persists across requests on a
 * keep-alive connection so that pipelined bytes (the start of
 * request N+1 already received while reading request N) are not
 * lost between handle_one_request calls. Without this, the
 * single-request implementation's "read up to \r\n\r\n then free"
 * pattern silently drops anything past the first request's body
 * boundary in the same recv. */
#define HTTP_CONN_BUF_CAP (16 * 1024)

/* The parser copies the request line into a fixed buffer; this is that size,
 * kept here so the check that answers 414 and the buffer that must hold the
 * line cannot drift apart. */
#define HTTP_MAX_REQUEST_LINE 2048

/* The parser copies each header line into a fixed buffer; same reasoning as
 * the request line above. */
#define HTTP_MAX_HEADER_LINE 1024

typedef struct HttpConn {
    int fd;
#ifdef AETHER_HAS_OPENSSL
    SSL* ssl;     /* non-NULL when this connection is TLS-wrapped */
#else
    void* ssl;    /* layout-stable placeholder for the no-TLS build */
#endif
    /* HTTP/2 (#260 Tier 2). Set to 1 either via ALPN selecting "h2"
     * during the TLS handshake or via an HTTP/1.1 → h2c upgrade
     * negotiated on a plain socket. When set, the request loop
     * routes every byte on this connection through the nghttp2
     * session wrapper rather than the HTTP/1.1 parser. */
    int   is_h2;
    /* Read-side ring: [read_pos, write_pos) holds bytes already
     * received but not yet consumed by a request parse. Bytes
     * before read_pos are spent and may be discarded by compaction;
     * bytes after write_pos are unallocated. */
    char* buf;
    int   buf_cap;
    int   read_pos;
    int   write_pos;
    /* Requests already served on this connection. A parked connection comes
     * back to a different worker, so the count has to live with the
     * connection rather than on the worker's stack (#1663). */
    int   requests_served;
    /* Peer and local address, resolved once per CONNECTION (#1719).
     *
     * These used to be fetched per request, on the reasoning that
     * getpeername/getsockname are cache-warm and therefore cheap. Measured
     * against nginx on the same box, that cost 2 syscalls, 2 inet_ntop calls
     * and 2 strdups on every request — nginx makes zero of any of them,
     * because neither address can change while a connection is open.
     *
     * Cached as text because that is the shape every consumer wants
     * (http_request_remote_addr returns a string). Empty string means the
     * lookup failed — a Unix-domain socket, or an fd that closed under us —
     * and is a valid cached answer, so `addrs_resolved` distinguishes
     * "looked and found nothing" from "not looked yet". Without that flag a
     * failing lookup would retry on every request, which is the cost this
     * removes.
     *
     * The request still owns its own copies: http_request_free frees
     * req->remote_addr and req->local_addr, so per-request strdups from
     * these are what get handed out. That keeps the ownership contract
     * unchanged; the saving is the syscalls and the inet_ntop, not the
     * allocation. */
    char  remote_addr[INET6_ADDRSTRLEN];
    char  local_addr[INET6_ADDRSTRLEN];
    int   remote_port;
    int   local_port;
    int   addrs_resolved;

    /* The SO_RCVTIMEO value currently on this socket, or -1 when none has been
     * applied yet (#1719).
     *
     * conn_serve applies the idle timeout on entry, and with connection parking
     * a kept-alive connection re-enters conn_serve once per request -- so an
     * unguarded apply is one setsockopt per request setting the value that is
     * already there. The comment on the parking path already claimed the window
     * was "only re-applied when it changes"; this is the guard that makes that
     * true. -1 rather than 0 because 0 is a valid "block indefinitely". */
    int   applied_recv_timeout_ms;
} HttpConn;

/* The parking lot holds HttpConn by pointer and needs exactly two things from
 * it: the descriptor to watch, and a way to close what it still owns. */
int http_conn_fd(HttpConn* conn) { return conn ? conn->fd : -1; }

static int conn_recv(HttpConn* c, void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (c->ssl) {
        int n = SSL_read(c->ssl, buf, len);
        if (n <= 0) return -1;
        return n;
    }
#endif
    return (int)recv(c->fd, buf, len, 0);
}

static int conn_send(HttpConn* c, const void* buf, int len) {
#ifdef AETHER_HAS_OPENSSL
    if (c->ssl) {
        int n = SSL_write(c->ssl, buf, len);
        if (n <= 0) return -1;
        return n;
    }
#endif
    return (int)send(c->fd, buf, len, 0);
}

/* Issue #383 zero-copy helpers. */

/* Coalesce headers + sendfile-emitted body into one TCP segment
 * (or as few as possible) by setting TCP_CORK / TCP_NOPUSH around
 * the header write + body sendfile. Without it, the kernel may
 * push a small headers-only segment immediately and follow with
 * separate body segments — measurable latency hit for small files.
 *
 * The two options are not the same contract, which matters for when the
 * cork is lifted. Clearing TCP_CORK on Linux sends what is queued. Clearing
 * TCP_NOPUSH on Darwin and the BSDs does not: the stack sends it when
 * something else prompts output, and TCP_NODELAY does not prompt it either
 * (measured). So on those platforms the cork is lifted BEFORE the body write
 * and the body write is what pushes headers and body together, which is the
 * coalescing this exists for. See conn_uncork_for_body.
 *
 * No-op on Windows (the fast path doesn't run there) and on
 * platforms without either socket option. Errors are non-fatal:
 * worst case the response still goes out, just less coalesced. */
static void conn_cork(HttpConn* c, int on) {
    if (!c || c->fd < 0) return;
#ifdef _WIN32
    (void)on;
    return;
#else
    int v = on ? 1 : 0;
#  if defined(TCP_CORK)
    setsockopt(c->fd, IPPROTO_TCP, TCP_CORK, (const void*)&v, sizeof(v));
#  elif defined(TCP_NOPUSH)
    setsockopt(c->fd, IPPROTO_TCP, TCP_NOPUSH, (const void*)&v, sizeof(v));
#  else
    (void)v;
#  endif
#endif
}

/* Lift the cork on the stacks where the following write is what pushes.
 *
 * Nothing prompted that write once a connection could be parked instead of
 * read from: a worker used to go straight back to recv() on the same socket
 * and the read path prompted the send, so a sendfile'd file reached the
 * client. A parked connection reads nothing until the client speaks, and the
 * client is waiting for this response, so the bytes sat in the kernel until
 * the connection closed — a 5-second stall for a static file on macOS, and a
 * hung request for any client that waits longer than it does. */
static void conn_uncork_for_body(HttpConn* c) {
#if !defined(_WIN32) && !defined(TCP_CORK) && defined(TCP_NOPUSH)
    conn_cork(c, 0);
#else
    (void)c;
#endif
}

/* Drain `size` bytes from `in_fd` to the connection's socket via
 * sendfile(2). Returns total bytes written on success, or -1 on
 * failure (caller should close the connection — partial sends
 * leave the wire in an inconsistent state for keep-alive).
 *
 * Returns -1 on platforms without sendfile (Windows, BSD variants
 * we haven't wired); the caller falls back to the buffered path.
 * Never invoked on TLS connections — handle_one_request gates the
 * call by checking conn->ssl == NULL. */
static long long conn_sendfile_drain(HttpConn* c, int in_fd, long long size) {
    if (!c || c->fd < 0 || in_fd < 0 || size < 0) return -1;
#if defined(__linux__)
    off_t off = 0;
    long long total = 0;
    while (total < size) {
        size_t want = (size_t)(size - total);
        ssize_t n = sendfile(c->fd, in_fd, &off, want);
        if (n > 0) {
            total += n;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Edge case for non-blocking sockets — cooperate
                 * with poll. The server's accepted sockets are
                 * blocking by default so this branch is rare. */
                struct pollfd pfd = { c->fd, POLLOUT, 0 };
                if (poll(&pfd, 1, 5000) <= 0) return -1;
                continue;
            }
            return -1;
        }
        /* n == 0: EOF on input or kernel said zero. Either way
         * we've sent all that's coming. */
        break;
    }
    return total;
#elif defined(__APPLE__)
    long long total = 0;
    while (total < size) {
        off_t len = (off_t)(size - total);
        int rc = sendfile(in_fd, c->fd, (off_t)total, &len, NULL, 0);
        /* macOS sendfile writes `len` bytes regardless of return
         * code; len is updated to the actual count. EINTR /
         * EAGAIN: partial; loop. Other errors: abort. */
        if (len > 0) total += (long long)len;
        if (rc == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd = { c->fd, POLLOUT, 0 };
            if (poll(&pfd, 1, 5000) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return total;
#else
    /* Windows / FreeBSD / unsupported. */
    (void)c; (void)in_fd; (void)size;
    return -1;
#endif
}

/* Returns 1 if the connection + request can take the sendfile fast
 * path: cleartext (no TLS), HTTP/1.1 (no h2 stream), and no Range
 * request (slice-aware sendfile is v2). */
static int sendfile_eligible(HttpConn* c, HttpRequest* req) {
    if (!c) return 0;
#ifdef AETHER_HAS_OPENSSL
    if (c->ssl) return 0;
#endif
    if (c->is_h2) return 0;
    if (req && req->header_keys && req->header_values) {
        for (int i = 0; i < req->header_count; i++) {
            if (req->header_keys[i] &&
                strcasecmp(req->header_keys[i], "Range") == 0) {
                return 0;
            }
        }
    }
    return 1;
}

/* Send `res` to `conn`. When res->sendfile_fd is set AND the
 * request/connection is sendfile-eligible, takes the zero-copy
 * path: serialize headers only (body is NULL on this path), cork
 * the socket, conn_send the headers, sendfile the body, uncork.
 * On ineligibility, reads the fd into res->body first then falls
 * through to the buffered serialize+send. Either way, owns and
 * closes the fd before returning.
 *
 * Returns 1 when the caller should force-close the connection
 * (sendfile partial-write, header-send failure, etc.); 0 when the
 * connection is safe to keep alive. */
static int send_response_with_optional_sendfile(HttpConn* conn,
                                                HttpServerResponse* res,
                                                HttpRequest* req) {
    if (!conn || !res) return 0;

    int force_close = 0;
    int sent_via_sendfile = 0;
    /* RFC 9110 section 9.3.2: a HEAD response carries the headers the GET
     * would have carried and no body. Sending one desynchronises a
     * persistent connection, the client reads those bytes as the start of
     * the next response. */
    int head_only = req && req->method && strcmp(req->method, "HEAD") == 0;

    if (head_only && res->sendfile_fd >= 0) {
        close(res->sendfile_fd);
        res->sendfile_fd = -1;
    }

    if (res->sendfile_fd >= 0 && sendfile_eligible(conn, req)) {
        size_t resp_len = 0;
        /* Serialize headers only — body is NULL on the fd path. */
        char* hdrs = http_response_serialize_len(res, &resp_len);
        if (hdrs) {
            conn_cork(conn, 1);
            int header_send = conn_send(conn, hdrs, (int)resp_len);
            free(hdrs);
            if (header_send < 0) {
                close(res->sendfile_fd);
                res->sendfile_fd = -1;
                force_close = 1;
            } else {
                conn_uncork_for_body(conn);
                long long sent = conn_sendfile_drain(conn, res->sendfile_fd,
                                                    res->sendfile_size);
                conn_cork(conn, 0);
                close(res->sendfile_fd);
                res->sendfile_fd = -1;
                if (sent != res->sendfile_size) {
                    /* Bytes written ≠ Content-Length — wire is
                     * inconsistent for keep-alive. Force close. */
                    force_close = 1;
                } else {
                    sent_via_sendfile = 1;
                }
            }
        }
    } else if (res->sendfile_fd >= 0) {
        /* Ineligible: read the fd into res->body before serializing.
         * The 2 GiB cap is conservative — buffered bodies above that
         * are unusual for static serves and would memory-press a
         * runtime that doesn't intend the cost. The fast path
         * (sendfile) has no such limit. */
        long long size = res->sendfile_size;
        if (size >= 0 && size <= (long long)0x7FFFFFFF) {
            /* Cap-aware (#343): file size is OS-supplied and
             * unbounded from the plugin host's perspective; gate via
             * the caps allocator. The matching free in
             * http_server_response_free passes res->body_cap. */
            size_t buf_cap = (size_t)size + 1;
            char* buf = (char*)aether_caps_malloc(buf_cap);
            if (buf) {
                size_t total = 0;
#ifndef _WIN32
                lseek(res->sendfile_fd, 0, SEEK_SET);
                while (total < (size_t)size) {
                    ssize_t n = read(res->sendfile_fd, buf + total,
                                     (size_t)size - total);
                    if (n <= 0) break;
                    total += (size_t)n;
                }
#endif
                if (total == (size_t)size) {
                    buf[size] = '\0';
                    aether_caps_free(res->body, res->body_cap);
                    res->body = buf;
                    res->body_length = (size_t)size;
                    res->body_cap = buf_cap;
                } else {
                    aether_caps_free(buf, buf_cap);
                }
            }
        }
#ifndef _WIN32
        close(res->sendfile_fd);
#endif
        res->sendfile_fd = -1;
    }

    if (!sent_via_sendfile) {
        size_t resp_len = 0;
        char*  body      = res->body;
        size_t body_len  = res->body_length;
        if (head_only) { res->body = NULL; res->body_length = 0; }
        char* response_str = http_response_serialize_len(res, &resp_len);
        if (head_only) { res->body = body; res->body_length = body_len; }
        if (response_str) {
            conn_send(conn, response_str, (int)resp_len);
            free(response_str);
        }
    }

    return force_close;
}

static void conn_close(HttpConn* c) {
#ifdef AETHER_HAS_OPENSSL
    if (c->ssl) {
        /* Best-effort graceful shutdown — ignore the result; we're
         * about to close the fd anyway and a half-closed peer is
         * not a server-side problem. */
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#endif
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    if (c->buf) {
        /* Cap-aware (#343): the per-connection read buffer is grown
         * via conn_buf_ensure (caps_realloc) and freed only here; its
         * exact size lives in c->buf_cap at both sites. Request sizes
         * drive the growth, so route it through the caps allocator. */
        aether_caps_free(c->buf, (size_t)c->buf_cap);
        c->buf = NULL;
        c->buf_cap = 0;
        c->read_pos = 0;
        c->write_pos = 0;
    }
}

/* Compact the read buffer: shift unconsumed bytes [read_pos,
 * write_pos) down to position 0 so the tail is available for a
 * fresh recv. Cheap when read_pos is large relative to write_pos. */
static void conn_buf_compact(HttpConn* c) {
    if (c->read_pos == 0) return;
    int unread = c->write_pos - c->read_pos;
    if (unread > 0) {
        memmove(c->buf, c->buf + c->read_pos, (size_t)unread);
    }
    c->write_pos = unread;
    c->read_pos = 0;
}

/* Grow the read buffer to at least the given capacity. */
static int conn_buf_ensure(HttpConn* c, int needed) {
    if (c->buf_cap >= needed) return 0;
    int new_cap = c->buf_cap > 0 ? c->buf_cap : HTTP_CONN_BUF_CAP;
    while (new_cap < needed) new_cap *= 2;
    /* Cap-aware (#343): old size is the prior c->buf_cap (0 on the
     * first grow from NULL); the matching free in conn_close passes
     * the same c->buf_cap. On cap-exceed caps_realloc returns NULL
     * and leaves the original buffer intact, which the NULL-check
     * below preserves. */
    char* nb = (char*)aether_caps_realloc(c->buf, (size_t)c->buf_cap,
                                          (size_t)new_cap);
    if (!nb) return -1;
    c->buf = nb;
    c->buf_cap = new_cap;
    return 0;
}

#ifdef AETHER_HAS_OPENSSL
/* ALPN selection callback (#260 Tier 2 — HTTP/2). The peer sends a
 * length-prefixed list of protocols it supports; we walk it and pick
 * the first one we recognise from our preference order
 * (h2 → http/1.1).
 *
 * Wire format per RFC 7301: a sequence of <len:u8><proto-name:bytes>
 * tuples. SSL_select_next_proto handles the parse for us; we just
 * supply our preference list in the same wire shape.
 *
 * `arg` is the HttpServer* registered when the SSL_CTX was created;
 * we honour its h2_enabled flag so a server that hasn't opted into
 * h2 always selects http/1.1 even when the peer offers h2. */
static int aether_alpn_select_cb(SSL* ssl,
                                 const unsigned char** out,
                                 unsigned char* outlen,
                                 const unsigned char* in,
                                 unsigned int inlen,
                                 void* arg) {
    (void)ssl;
    HttpServer* server = (HttpServer*)arg;

    /* Wire-format ALPN advertisement, h2 first when the server
     * enabled it. Each entry is <length:u8><name:bytes>. */
    static const unsigned char alpn_h2_first[] = {
        2, 'h', '2',
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'
    };
    static const unsigned char alpn_http11_only[] = {
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'
    };
    const unsigned char* prefs;
    unsigned int prefs_len;
    if (server && server->h2_enabled) {
        prefs = alpn_h2_first;
        prefs_len = sizeof(alpn_h2_first);
    } else {
        prefs = alpn_http11_only;
        prefs_len = sizeof(alpn_http11_only);
    }

    /* SSL_select_next_proto returns OPENSSL_NPN_NEGOTIATED on a hit
     * and OPENSSL_NPN_NO_OVERLAP otherwise. The function still writes
     * a fallback proto into *out / *outlen when there's no overlap;
     * we treat that case as a hard failure so the handshake aborts —
     * better to fail loudly than to silently downgrade the
     * negotiation against the client's preferences. */
    int rc = SSL_select_next_proto((unsigned char**)out, outlen,
                                   prefs, prefs_len, in, inlen);
    if (rc == OPENSSL_NPN_NEGOTIATED) return SSL_TLSEXT_ERR_OK;
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* TLS-wrap an accepted fd. Returns 0 on success (conn->ssl set), -1 on
 * handshake failure (caller should close conn->fd and discard).
 * After the handshake, queries the negotiated ALPN protocol so the
 * connection knows whether to drive HTTP/1.1 or HTTP/2 on the wire. */
static int conn_tls_accept(HttpConn* conn, SSL_CTX* ctx) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) return -1;
    if (SSL_set_fd(ssl, conn->fd) != 1) {
        SSL_free(ssl);
        return -1;
    }
    int r = SSL_accept(ssl);
    if (r <= 0) {
        /* Handshake failed — peer probably spoke plain HTTP at a TLS
         * port, or sent an unsupported cipher. Drain OpenSSL's error
         * queue so the next handshake on this thread starts clean. */
        ERR_clear_error();
        SSL_free(ssl);
        return -1;
    }
    conn->ssl = ssl;

    /* Detect h2 negotiation. SSL_get0_alpn_selected returns the proto
     * name + length via out-pointers; len==0 means no ALPN was used
     * (peer didn't offer the extension), in which case we stay on
     * HTTP/1.1 by default. */
    const unsigned char* alpn = NULL;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len == 2 && alpn && alpn[0] == 'h' && alpn[1] == '2') {
        conn->is_h2 = 1;
    }
    return 0;
}

/* OpenSSL global init — called once on first http_server_set_tls. The
 * defaults (TLS 1.2 minimum, all known ciphers) match the existing
 * client-side context in std/net/aether_http.c. */
static void server_openssl_init_once(void) {
    static int done = 0;
    if (done) return;
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    done = 1;
}
#endif

// Portable case-insensitive substring search (strcasestr is a GNU extension)
static const char* http_strcasestr(const char* haystack, const char* needle) {
    if (!needle || !*needle) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            size_t i;
            for (i = 1; i < nlen; i++) {
                if (tolower((unsigned char)haystack[i]) != tolower((unsigned char)needle[i]))
                    break;
            }
            if (i == nlen) return haystack;
        }
    }
    return NULL;
}

static int http_server_initialized = 0;

static void http_server_init() {
    if (http_server_initialized) return;
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    http_server_initialized = 1;
}

HttpServer* http_server_create(int port) {
    http_server_init();

    HttpServer* server = (HttpServer*)calloc(1, sizeof(HttpServer));
    server->port = port;
    server->host = strdup("0.0.0.0");
    server->socket_fd = -1;
    server->is_running = 0;
    server->routes = NULL;
    server->middleware_chain = NULL;
    server->max_connections = 1000;
    server->keep_alive_timeout = 30;
    server->scheduler = NULL;
    server->handler_actor = NULL;
    server->send_fn = NULL;
    server->spawn_fn = NULL;
    server->release_fn = NULL;
    server->step_fn = NULL;
    server->park_lot = NULL;
    server->conn_pool = NULL;
    server->accept_poller.fd = -1;
    server->accept_poller.backend_data = NULL;
    server->multi_accept = 0;
    server->accept_thread_count = 0;
    server->accept_threads = NULL;
    server->accept_listen_fds = NULL;
    server->accept_pollers = NULL;
    server->tls_enabled = 0;
    server->tls_ctx = NULL;
    server->h2_enabled = 0;
    server->h2_max_concurrent_streams = 0;  /* nghttp2 default 100 when 0 */
    /* HTTP/1.1 is persistent by default (RFC 9112 section 9.3), and a proxy
     * or client that dials this server pays a handshake per request when it
     * is not: the load balancer measured 3.7x behind nginx with 8% of
     * requests dropped mid-response, entirely from connection churn (#1653).
     * A client that asks for `Connection: close` still gets close, and the
     * response path refuses to keep a connection whose body has no definite
     * length. 0 = unlimited requests, 0 = the 30s idle default. */
    server->keep_alive_enabled = 1;
    server->keep_alive_max = 0;
    server->keep_alive_idle_ms = 0;
    server->response_transformer_chain = NULL;
    server->on_start = NULL;
    server->on_start_user_data = NULL;
    server->on_stop = NULL;
    server->on_stop_user_data = NULL;
    server->ready_check = NULL;
    server->ready_check_user_data = NULL;
    atomic_init(&server->inflight_connections, 0);
    server->request_hook_chain = NULL;
    server->sse_routes = NULL;
    server->ws_routes = NULL;

    return server;
}

const char* http_server_set_keepalive_raw(HttpServer* server,
                                          int enabled,
                                          int max_requests,
                                          int64_t idle_ns) {
    if (!server) return "server is null";
    server->keep_alive_enabled = enabled ? 1 : 0;
    server->keep_alive_max = max_requests < 0 ? 0 : max_requests;
    /* Storage is ms-granular (the keepalive poll/timeout paths run
     * in ms). Sub-ms keepalive intervals would be silly; round down
     * to ms here so we don't carry false precision. */
    int64_t idle_ms = idle_ns / 1000000LL;
    if (idle_ms < 0) idle_ms = 0;
    if (idle_ms > INT_MAX) idle_ms = INT_MAX;
    server->keep_alive_idle_ms = (int)idle_ms;
    return "";
}

// =================================================================
// #260 Tier 3: graceful shutdown + lifecycle hooks + health probes
// =================================================================

void http_server_set_on_start(HttpServer* server, HttpLifecycleHook hook, void* user_data) {
    if (!server) return;
    server->on_start = hook;
    server->on_start_user_data = user_data;
}

void http_server_set_on_stop(HttpServer* server, HttpLifecycleHook hook, void* user_data) {
    if (!server) return;
    server->on_stop = hook;
    server->on_stop_user_data = user_data;
}

const char* http_server_shutdown_graceful_raw(HttpServer* server, int64_t timeout_ns) {
    if (!server) return "server is null";

    /* Stop accepting new connections — this unblocks the accept
     * loop's poll() and lets it exit. The thread-pool destructor
     * (or actor scheduler shutdown) handles in-flight connections;
     * we just wait for the inflight counter to drain. */
    http_server_stop(server);

    /* The drain spin-wait is ms-granular; round down ns→ms.
     * 0 or negative reverts to the previous 5s default. */
    int64_t timeout_ms_64 = timeout_ns / 1000000LL;
    if (timeout_ms_64 > INT_MAX) timeout_ms_64 = INT_MAX;
    int timeout_ms = (int)timeout_ms_64;
    if (timeout_ms <= 0) timeout_ms = 5000;

    /* Spin-wait with an exponential-ish back-off, capped at 50ms.
     * The connection counter is updated atomically by every
     * http_server_drain_connection invocation (Tier 0 / Phase C3
     * helper); when it reaches zero, all in-flight responses have
     * completed naturally. */
    int waited = 0;
    int sleep_us = 1000;  /* 1 ms */
    while (waited < timeout_ms) {
        int n = atomic_load(&server->inflight_connections);
        if (n <= 0) return "";
#ifdef _WIN32
        Sleep(sleep_us / 1000 ? sleep_us / 1000 : 1);
#else
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)sleep_us * 1000L };
        nanosleep(&ts, NULL);
#endif
        waited += sleep_us / 1000;
        if (sleep_us < 50000) sleep_us *= 2;
    }
    return "timeout";
}

/* Shared stateless handler for /healthz — always 200. */
static void health_live_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    http_response_set_status(res, 200);
    http_response_set_header(res, "Content-Type", "text/plain");
    http_response_set_body(res, "ok");
}

/* Shared stateless handler for /readyz — calls server->ready_check
 * (passed via the route's user_data slot — we stash the
 * server pointer there). */
static void health_ready_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req;
    HttpServer* server = (HttpServer*)ud;
    int ok = 1;
    if (server && server->ready_check) {
        ok = server->ready_check(server->ready_check_user_data);
    }
    if (ok) {
        http_response_set_status(res, 200);
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res, "ready");
    } else {
        http_response_set_status(res, 503);
        http_response_set_header(res, "Content-Type", "text/plain");
        http_response_set_body(res, "not ready");
    }
}

const char* http_server_set_health_probes_raw(HttpServer* server,
                                              const char* live_path,
                                              const char* ready_path,
                                              HttpReadyCheck ready_check,
                                              void* user_data) {
    if (!server) return "server is null";
    server->ready_check = ready_check;
    server->ready_check_user_data = user_data;
    if (live_path && *live_path) {
        http_server_get(server, live_path, health_live_handler, NULL);
    }
    if (ready_path && *ready_path) {
        http_server_get(server, ready_path, health_ready_handler, server);
    }
    return "";
}

// =================================================================
// #260 Tier 3 / F1: access logger
// =================================================================

typedef struct {
    char* format;       /* "combined" or "json" */
    FILE* fp;           /* not closed by us — that's set up below */
    int   own_fp;       /* 1 if we opened it (need to fclose); 0 for stderr */
} AccessLogState;

/* Format a Common/Combined Log Format timestamp: "DD/MMM/YYYY:HH:MM:SS +0000".
 *
 * Written by hand rather than with strftime's %b, which emits the *locale's*
 * abbreviated month name. CLF's month is defined as the English three-letter
 * abbreviation — this is a parseable interchange format (GoAccess, AWStats,
 * Logstash, Splunk), not human-facing text. An Aether server embedded in a host
 * that had called setlocale(LC_ALL, "") would otherwise write "08/Mär/2026" and
 * break every downstream parser, and unlike a bad response the damage lands in
 * archived logs where nothing round-trips it to reveal the corruption.
 *
 * Same class of bug as the LC_NUMERIC float conversions fixed in #1459. A static
 * table removes the locale dependency outright rather than pinning a locale
 * around the call, which is the right trade when the format is specified in
 * terms of English names rather than "the C locale's names".
 *
 * Not static: covered directly by tests/runtime/test_http_server.c. */
void http_format_clf_time(char* out, size_t out_size, const struct tm* tmv) {
    static const char* const kMonthsEn[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (!out || out_size == 0) return;
    if (!tmv) { out[0] = '\0'; return; }
    int mon = (tmv->tm_mon >= 0 && tmv->tm_mon < 12) ? tmv->tm_mon : 0;
    snprintf(out, out_size, "%02d/%s/%04d:%02d:%02d:%02d +0000",
             tmv->tm_mday, kMonthsEn[mon], tmv->tm_year + 1900,
             tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
}

static void access_log_hook(HttpRequest* req, HttpServerResponse* res,
                            long duration_us, void* user_data) {
    AccessLogState* st = (AccessLogState*)user_data;
    if (!st || !st->fp) return;

    /* Common fields. Avoid NULL-deref by substituting "-" the way
     * NCSA log files traditionally do. */
    const char* method = req && req->method ? req->method : "-";
    const char* path   = req && req->path   ? req->path   : "-";
    const char* version = req && req->http_version ? req->http_version : "HTTP/1.1";
    const char* user_agent = req ? http_get_header(req, "User-Agent") : NULL;
    const char* referer    = req ? http_get_header(req, "Referer")    : NULL;
    int  status = res ? res->status_code : 0;
    long body_len = res ? (long)res->body_length : 0;

    /* RFC 1123 date for combined; ISO-8601 for json. Both via the
     * same struct tm pull. */
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif

    if (strcmp(st->format, "json") == 0) {
        char ts[32];
        /* Numeric-only conversions: no locale-sensitive specifier here, so
         * plain strftime is safe. Do NOT add %b/%a/%p to this format — they
         * are locale-dependent and this string goes on the wire. */
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        fprintf(st->fp,
                "{\"ts\":\"%s\",\"method\":\"%s\",\"path\":\"%s\","
                "\"status\":%d,\"bytes\":%ld,\"dur_us\":%ld,"
                "\"ua\":\"%s\",\"ref\":\"%s\"}\n",
                ts, method, path, status, body_len, duration_us,
                user_agent ? user_agent : "",
                referer    ? referer    : "");
    } else {
        /* Combined: <ip> - - [DD/MMM/YYYY:HH:MM:SS +0000] "METHOD PATH HTTP/X.Y" status bytes "REF" "UA" */
        char ts[64];
        http_format_clf_time(ts, sizeof(ts), &tmv);
        const char* ip = req ? http_get_header(req, "X-Forwarded-For") : NULL;
        if (!ip) ip = "-";
        fprintf(st->fp,
                "%s - - [%s] \"%s %s %s\" %d %ld \"%s\" \"%s\"\n",
                ip, ts, method, path, version, status, body_len,
                referer    ? referer    : "-",
                user_agent ? user_agent : "-");
    }
    fflush(st->fp);
}

const char* http_server_set_access_log_raw(HttpServer* server,
                                           const char* format,
                                           const char* output_path) {
    if (!server) return "server is null";
    if (!format || !*format) return "";  /* disabled */

    int fmt_ok = strcmp(format, "combined") == 0 ||
                 strcmp(format, "json")     == 0;
    if (!fmt_ok) return "format must be \"combined\" or \"json\"";

    AccessLogState* st = (AccessLogState*)calloc(1, sizeof(AccessLogState));
    if (!st) return "out of memory";
    st->format = strdup(format);

    if (!output_path || !*output_path || strcmp(output_path, "-") == 0) {
        st->fp = stderr;
        st->own_fp = 0;
    } else {
        st->fp = fopen(output_path, "ab");
        if (!st->fp) {
            free(st->format);
            free(st);
            return "cannot open access-log output_path for append";
        }
        st->own_fp = 1;
    }

    http_server_use_request_hook(server, access_log_hook, st);
    return "";
}

// =================================================================
// #260 Tier 3 / F2: per-route metrics + Prometheus exposition
// =================================================================

typedef struct MetricsRoute {
    char* method;
    char* path_pattern;
    _Atomic long total_requests;
    _Atomic long total_errors;     /* status >= 500 */
    _Atomic long total_4xx;
    _Atomic long sum_duration_us;
    _Atomic long max_duration_us;
    /* Histogram buckets in microseconds. */
    _Atomic long bucket_le_5ms;
    _Atomic long bucket_le_25ms;
    _Atomic long bucket_le_100ms;
    _Atomic long bucket_le_500ms;
    _Atomic long bucket_le_2s;
    _Atomic long bucket_le_10s;
    struct MetricsRoute* next;
} MetricsRoute;

typedef struct {
    pthread_mutex_t lock;
    MetricsRoute* head;
    /* The server this scrape belongs to, for the gauges that read live state
     * rather than accumulated counters. */
    HttpServer* server;
} MetricsState;

static MetricsRoute* metrics_route_for(MetricsState* st,
                                       const char* method,
                                       const char* pattern) {
    MetricsRoute* r = st->head;
    while (r) {
        if (strcmp(r->method, method) == 0 &&
            strcmp(r->path_pattern, pattern) == 0) return r;
        r = r->next;
    }
    return NULL;
}

static void metrics_hook(HttpRequest* req, HttpServerResponse* res,
                         long duration_us, void* user_data) {
    MetricsState* st = (MetricsState*)user_data;
    if (!st || !req || !res) return;

    /* Look up by exact method+path. Production deployments would
     * usually want path-pattern bucketing (e.g. /users/:id collapses
     * across IDs), but the existing route table doesn't expose the
     * matched pattern back to the dispatch path. v1 buckets by
     * literal request path; the user can collapse via labels in a
     * follow-up. */
    const char* method = req->method ? req->method : "-";
    const char* pattern = req->path ? req->path : "-";

    pthread_mutex_lock(&st->lock);
    MetricsRoute* r = metrics_route_for(st, method, pattern);
    if (!r) {
        r = (MetricsRoute*)calloc(1, sizeof(MetricsRoute));
        if (!r) { pthread_mutex_unlock(&st->lock); return; }
        r->method = strdup(method);
        r->path_pattern = strdup(pattern);
        r->next = st->head;
        st->head = r;
    }
    pthread_mutex_unlock(&st->lock);

    atomic_fetch_add(&r->total_requests, 1);
    if (res->status_code >= 500) atomic_fetch_add(&r->total_errors, 1);
    else if (res->status_code >= 400) atomic_fetch_add(&r->total_4xx, 1);
    atomic_fetch_add(&r->sum_duration_us, duration_us);

    /* Update max via CAS. */
    long prev = atomic_load(&r->max_duration_us);
    while (duration_us > prev &&
           !atomic_compare_exchange_weak(&r->max_duration_us, &prev, duration_us)) {
        /* prev is updated by CAS on failure; re-test loop condition. */
    }

    /* Cumulative histogram (Prometheus convention: each bucket
     * counts events <= upper bound). */
    if (duration_us <= 5000)    atomic_fetch_add(&r->bucket_le_5ms,    1);
    if (duration_us <= 25000)   atomic_fetch_add(&r->bucket_le_25ms,   1);
    if (duration_us <= 100000)  atomic_fetch_add(&r->bucket_le_100ms,  1);
    if (duration_us <= 500000)  atomic_fetch_add(&r->bucket_le_500ms,  1);
    if (duration_us <= 2000000) atomic_fetch_add(&r->bucket_le_2s,     1);
    if (duration_us <= 10000000)atomic_fetch_add(&r->bucket_le_10s,    1);
}

/* Escape a pattern for use as a Prometheus label value. */
static void metrics_escape(char* dst, size_t cap, const char* src) {
    size_t n = 0;
    for (; *src && n + 2 < cap; src++) {
        if (*src == '"' || *src == '\\') {
            if (n + 2 >= cap) break;
            dst[n++] = '\\';
            dst[n++] = *src;
        } else if (*src == '\n') {
            if (n + 2 >= cap) break;
            dst[n++] = '\\';
            dst[n++] = 'n';
        } else {
            dst[n++] = *src;
        }
    }
    dst[n] = '\0';
}

/* Handler for the configured /metrics endpoint. Walks the
 * per-route counters and emits Prometheus text format. */
static void metrics_handler(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)req;
    MetricsState* st = (MetricsState*)user_data;
    if (!st) {
        http_response_set_status(res, 500);
        http_response_set_body(res, "metrics: state missing");
        return;
    }

    /* Build into a heap buffer; size grows with route count. 64KB
     * suffices for hundreds of routes. Cap-aware (#343): the buffer
     * is allocated and freed entirely within this handler, so the
     * exact byte count (`cap`) is in scope at the matching free
     * below — route it through the caps allocator so a plugin host's
     * memory budget bounds the /metrics scrape allocation. */
    size_t cap = 64 * 1024;
    char* buf = (char*)aether_caps_malloc(cap);
    if (!buf) {
        http_response_set_status(res, 500);
        http_response_set_body(res, "metrics: oom");
        return;
    }
    size_t off = 0;
    off += snprintf(buf + off, cap - off,
        "# TYPE aether_http_requests_total counter\n"
        "# TYPE aether_http_errors_total counter\n"
        "# TYPE aether_http_4xx_total counter\n"
        "# TYPE aether_http_request_duration_seconds histogram\n"
        "# TYPE aether_http_parked_connections gauge\n");

    /* Idle keep-alive connections held by the poller rather than by a worker
     * (#1663). Worth exporting because it is the number that used to be
     * capped at the worker count: a deployment can see how far past that line
     * it is running. */
    off += snprintf(buf + off, cap - off,
        "aether_http_parked_connections %d\n",
        st->server ? http_park_count((HttpParkLot*)st->server->park_lot) : 0);

    pthread_mutex_lock(&st->lock);
    for (MetricsRoute* r = st->head; r; r = r->next) {
        char m[64], p[256];
        metrics_escape(m, sizeof(m), r->method);
        metrics_escape(p, sizeof(p), r->path_pattern);
        long total = atomic_load(&r->total_requests);
        long errs  = atomic_load(&r->total_errors);
        long c4xx  = atomic_load(&r->total_4xx);
        long sum   = atomic_load(&r->sum_duration_us);
        long b5    = atomic_load(&r->bucket_le_5ms);
        long b25   = atomic_load(&r->bucket_le_25ms);
        long b100  = atomic_load(&r->bucket_le_100ms);
        long b500  = atomic_load(&r->bucket_le_500ms);
        long b2    = atomic_load(&r->bucket_le_2s);
        long b10   = atomic_load(&r->bucket_le_10s);
        int wrote = snprintf(buf + off, cap - off,
            "aether_http_requests_total{method=\"%s\",path=\"%s\"} %ld\n"
            "aether_http_errors_total{method=\"%s\",path=\"%s\"} %ld\n"
            "aether_http_4xx_total{method=\"%s\",path=\"%s\"} %ld\n"
            "aether_http_request_duration_seconds_bucket{method=\"%s\",path=\"%s\",le=\"0.005\"} %ld\n"
            "aether_http_request_duration_seconds_bucket{method=\"%s\",path=\"%s\",le=\"0.025\"} %ld\n"
            "aether_http_request_duration_seconds_bucket{method=\"%s\",path=\"%s\",le=\"0.1\"} %ld\n"
            "aether_http_request_duration_seconds_bucket{method=\"%s\",path=\"%s\",le=\"0.5\"} %ld\n"
            "aether_http_request_duration_seconds_bucket{method=\"%s\",path=\"%s\",le=\"2\"} %ld\n"
            "aether_http_request_duration_seconds_bucket{method=\"%s\",path=\"%s\",le=\"10\"} %ld\n"
            "aether_http_request_duration_seconds_bucket{method=\"%s\",path=\"%s\",le=\"+Inf\"} %ld\n"
            "aether_http_request_duration_seconds_sum{method=\"%s\",path=\"%s\"} %.6f\n"
            "aether_http_request_duration_seconds_count{method=\"%s\",path=\"%s\"} %ld\n",
            m, p, total,
            m, p, errs,
            m, p, c4xx,
            m, p, b5, m, p, b25, m, p, b100, m, p, b500,
            m, p, b2, m, p, b10, m, p, total,
            m, p, (double)sum / 1e6,
            m, p, total);
        if (wrote < 0 || (size_t)wrote >= cap - off) break;
        off += (size_t)wrote;
    }
    pthread_mutex_unlock(&st->lock);

    http_response_set_status(res, 200);
    http_response_set_header(res, "Content-Type", "text/plain; version=0.0.4");
    http_response_set_body(res, buf);
    aether_caps_free(buf, cap);
}

const char* http_server_set_metrics_raw(HttpServer* server,
                                        const char* metrics_endpoint) {
    if (!server) return "server is null";
    MetricsState* st = (MetricsState*)calloc(1, sizeof(MetricsState));
    if (!st) return "out of memory";
    pthread_mutex_init(&st->lock, NULL);
    st->server = server;
    http_server_use_request_hook(server, metrics_hook, st);
    const char* endpoint = (metrics_endpoint && *metrics_endpoint)
        ? metrics_endpoint : "/metrics";
    http_server_get(server, endpoint, metrics_handler, st);
    return "";
}

const char* http_server_set_tls_raw(HttpServer* server,
                                    const char* cert_path,
                                    const char* key_path) {
    if (!server) return "server is null";
    if (!cert_path || !*cert_path) return "cert_path is empty";
    if (!key_path  || !*key_path)  return "key_path is empty";
#ifdef AETHER_HAS_OPENSSL
    server_openssl_init_once();

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return "SSL_CTX_new failed";
    /* Match the client side: TLS 1.2+ only. Older versions are
     * known-broken (POODLE, etc.) and there's no compat reason to
     * keep them around for an in-process server. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* Disable legacy compression and renegotiation to remove the
     * remaining historical attack surface. */
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION |
                              SSL_OP_NO_RENEGOTIATION);

    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return "failed to load TLS certificate (check cert_path and PEM format)";
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return "failed to load TLS private key (check key_path and PEM format)";
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return "TLS cert and private key do not match";
    }

    /* ALPN selection callback (#260 Tier 2). The callback inspects
     * server->h2_enabled at handshake time, so toggling
     * http_server_set_h2 between calls actually changes the
     * advertised list — the SSL_CTX itself doesn't have to be
     * rebuilt. */
    SSL_CTX_set_alpn_select_cb(ctx, aether_alpn_select_cb, server);

    /* Replace any prior context (idempotent re-load). */
    if (server->tls_ctx) {
        SSL_CTX_free((SSL_CTX*)server->tls_ctx);
    }
    server->tls_ctx = ctx;
    server->tls_enabled = 1;
    return "";
#else
    (void)cert_path; (void)key_path;
    return "TLS unavailable: built without OpenSSL";
#endif
}

/* Enable HTTP/2 on this server (#260 Tier 2). Once enabled:
 *   - If TLS is also enabled, the ALPN callback advertises "h2" first
 *     and falls back to "http/1.1" — clients that don't speak h2
 *     keep working unchanged.
 *   - Plain (non-TLS) connections honour h2c upgrade requests
 *     (RFC 7540 §3.2): an HTTP/1.1 client sending Upgrade: h2c +
 *     HTTP2-Settings: <base64> gets a 101 Switching Protocols and
 *     transitions to HTTP/2 framed mode.
 *   - Streams demux into the existing route table, so middleware,
 *     metrics, access logs, and health probes all apply uniformly.
 *
 * Returns "" on success, an error string when nghttp2 isn't linked.
 * max_concurrent_streams = 0 → use libnghttp2's default (100). */
const char* http_server_set_h2_raw(HttpServer* server,
                                   int max_concurrent_streams) {
    if (!server) return "server is null";
    if (max_concurrent_streams < 0) {
        return "max_concurrent_streams must be >= 0";
    }
#ifdef AETHER_HAS_NGHTTP2
    server->h2_enabled = 1;
    server->h2_max_concurrent_streams = max_concurrent_streams;
    return "";
#else
    (void)max_concurrent_streams;
    return "HTTP/2 unavailable: built without libnghttp2";
#endif
}

const char* http_server_set_h2_concurrent_dispatch_raw(HttpServer* server,
                                                       int worker_count) {
    if (!server) return "server is null";
    if (worker_count < 0) return "worker_count must be >= 0";
    if (worker_count > 64) return "worker_count must be <= 64";
#ifdef AETHER_HAS_NGHTTP2
    server->h2_dispatch_workers = worker_count;
    return "";
#else
    return "HTTP/2 unavailable: built without libnghttp2";
#endif
}

void http_server_set_host(HttpServer* server, const char* host) {
    if (!server || !host || !*host) return;
    char* copy = strdup(host);
    if (!copy) return;
    if (server->host) free(server->host);
    server->host = copy;
}

int http_server_bind_raw(HttpServer* server, const char* host, int port) {
    server->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket_fd < 0) {
        fprintf(stderr, "Failed to create socket\n");
        return -1;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(server->socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host, &addr.sin_addr);
    }
    
    if (bind(server->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to bind socket to %s:%d\n", host, port);
        close(server->socket_fd);
        server->socket_fd = -1;
        return -1;
    }
    
    if (listen(server->socket_fd, server->max_connections) < 0) {
        fprintf(stderr, "Failed to listen on socket\n");
        close(server->socket_fd);
        server->socket_fd = -1;
        return -1;
    }
    
    // Update host and port (make copy of host string first)
    char* new_host = strdup(host);
    if (server->host) {
        free(server->host);
    }
    server->host = new_host;
    server->port = port;

    // Resolve an OS-assigned port (port 0): the kernel picked a real
    // port at bind() time, but `port` is still 0 here. getsockname() the
    // bound socket and write the actual port back so dynamic-port users
    // (parallel test runners, the VCR embed layer) can read it via
    // http_server_port(). vcr_embed_abi_wish.md open question 3.
    if (port == 0) {
        struct sockaddr_in bound;
        socklen_t blen = sizeof(bound);
        if (getsockname(server->socket_fd, (struct sockaddr*)&bound, &blen) == 0) {
            server->port = ntohs(bound.sin_port);
        }
    }

    return 0;
}

// Resolved listening port. For a server bound with port 0 this is the
// OS-assigned port (see the getsockname() in http_server_bind_raw);
// otherwise it echoes the requested port. Returns 0 if unbound.
int http_server_port(HttpServer* server) {
    return server ? server->port : 0;
}

// Request parsing
//
// Two entry points:
//
//   http_parse_request_n(buf, len) — length-aware. `len` is the
//     authoritative byte count of the request slice; the body parse
//     clamps `Content-Length` against the remaining bytes after the
//     header block, so a client-supplied `Content-Length: 99999` with
//     a 19-byte body no longer crashes via memcpy-overread (the bug
//     ASan caught in PR #532 on the pre-existing
//     `http_server_post_with_body` fixture).
//
//   http_parse_request(raw) — thin wrapper for legacy text-shaped
//     callers; calls `_n(raw, strlen(raw))`.
//
// Implementation note: the request-line and header parsing still use
// strstr, which means `buf` must be NUL-terminated at offset `len`
// (the wire-protocol caller already NUL-pokes its receive buffer for
// this reason; the wrapper above terminates by construction). `len`
// is consulted only for the body-bounds clamp — exactly the place
// where binary payloads with embedded NULs need it.
HttpRequest* http_parse_request_n(const char* buf, size_t len) {
    if (!buf) return NULL;
    const char* raw_request = buf;
    HttpRequest* req = (HttpRequest*)calloc(1, sizeof(HttpRequest));

    // Parse request line: METHOD /path HTTP/1.1
    const char* line_end = strstr(raw_request, "\r\n");
    if (!line_end) {
        free(req);
        return NULL;
    }
    
    /* The request line is attacker-controlled and arbitrarily long: a URL
     * longer than this buffer used to be copied into it anyway, off the end
     * of the stack frame, which any client could do with one long path. The
     * caller rejects an over-long line with 414 before reaching here; this
     * bound is the one that has to hold whoever the caller turns out to be. */
    char request_line[HTTP_MAX_REQUEST_LINE];
    size_t line_len = (size_t)(line_end - raw_request);
    if (line_len >= sizeof(request_line)) {
        free(req);
        return NULL;
    }
    memcpy(request_line, raw_request, line_len);
    request_line[line_len] = '\0';
    
    // Extract method
    char* space = strchr(request_line, ' ');
    if (!space) {
        free(req);
        return NULL;
    }
    
    int method_len = space - request_line;
    req->method = (char*)malloc(method_len + 1);
    if (!req->method) { free(req); return NULL; }
    memcpy(req->method, request_line, method_len);
    req->method[method_len] = '\0';

    // Extract path and query string
    char* path_start = space + 1;
    char* path_end = strchr(path_start, ' ');
    if (!path_end) {
        free(req->method);
        free(req);
        return NULL;
    }

    char* query = strchr(path_start, '?');
    if (query && query < path_end) {
        // Has query string
        int path_len = query - path_start;
        req->path = (char*)malloc(path_len + 1);
        if (!req->path) { free(req->method); free(req); return NULL; }
        memcpy(req->path, path_start, path_len);
        req->path[path_len] = '\0';

        int query_len = path_end - query - 1;
        req->query_string = (char*)malloc(query_len + 1);
        if (!req->query_string) {
            free(req->path); free(req->method); free(req); return NULL;
        }
        memcpy(req->query_string, query + 1, query_len);
        req->query_string[query_len] = '\0';
    } else {
        // No query string
        int path_len = path_end - path_start;
        req->path = (char*)malloc(path_len + 1);
        if (!req->path) { free(req->method); free(req); return NULL; }
        memcpy(req->path, path_start, path_len);
        req->path[path_len] = '\0';
        req->query_string = NULL;
    }
    
    // Extract HTTP version
    char* version_start = path_end + 1;
    req->http_version = strdup(version_start);
    
    // Parse headers
    req->header_keys = (char**)malloc(sizeof(char*) * 50);
    req->header_values = (char**)malloc(sizeof(char*) * 50);
    req->header_count = 0;
    // If either array failed to allocate, drop both and skip storing headers
    // (the loop below still runs to advance past them to the body) rather than
    // writing strdup results through a NULL array pointer.
    if (!req->header_keys || !req->header_values) {
        free(req->header_keys);   req->header_keys = NULL;
        free(req->header_values); req->header_values = NULL;
    }

    const char* header_start = line_end + 2;
    while (1) {
        line_end = strstr(header_start, "\r\n");
        if (!line_end || line_end == header_start) {
            // End of headers
            if (line_end) {
                header_start = line_end + 2;
            }
            break;
        }
        
        /* A header line is attacker-controlled and arbitrarily long. Copying
         * one longer than this buffer wrote off the end of the stack frame,
         * which any client could do with one long header value. The caller
         * answers 431 before reaching here; this bound is what holds if some
         * other caller does not. */
        char header_line[HTTP_MAX_HEADER_LINE];
        line_len = (size_t)(line_end - header_start);
        if (line_len >= sizeof(header_line)) break;
        memcpy(header_line, header_start, line_len);
        header_line[line_len] = '\0';
        
        char* colon = strchr(header_line, ':');
        if (colon && req->header_count < 50 && req->header_keys && req->header_values) {
            *colon = '\0';
            char* key = header_line;
            char* value = colon + 1;

            // Trim whitespace from value
            while (*value == ' ') value++;

            // Store only if both copies succeed; a NULL key/value would later
            // crash header lookup (strcasecmp) or serialization (strlen).
            char* kd = strdup(key);
            char* vd = strdup(value);
            if (kd && vd) {
                req->header_keys[req->header_count] = kd;
                req->header_values[req->header_count] = vd;
                req->header_count++;
            } else {
                free(kd);
                free(vd);
            }
        }
        
        header_start = line_end + 2;
    }
    
    // Parse body. Binary bodies (Content-Encoding: x-lzf, gzip, etc., or
    // any application/octet-stream payload) may contain embedded NUL
    // bytes. The caller (see http_server_accept_loop's request-buffer
    // assembly) reads Content-Length bytes off the wire and NUL-
    // terminates the slice for header parsing, so the body bytes are
    // present at *header_start* — they just don't survive a strdup/strlen
    // pair. Honour the parsed Content-Length header to memcpy the exact
    // byte count, clamped against `body_avail` (what's actually buffered
    // — defends against a malicious or buggy client lying with an
    // oversized Content-Length, which would otherwise memcpy past the
    // buffer). Fall back to strdup/strlen only when Content-Length is
    // absent (HTTP/1.0 text bodies, malformed client requests).
    long body_cl = -1;
    for (int hi = 0; hi < req->header_count; hi++) {
        if (req->header_keys[hi] &&
            strcasecmp(req->header_keys[hi], "Content-Length") == 0 &&
            req->header_values[hi]) {
            body_cl = strtol(req->header_values[hi], NULL, 10);
            if (body_cl < 0) body_cl = -1;
            break;
        }
    }
    size_t body_offset = (size_t)(header_start - raw_request);
    size_t body_avail = (body_offset <= len) ? (len - body_offset) : 0;
    if (body_cl >= 0) {
        size_t copy_n = (size_t)body_cl;
        if (copy_n > body_avail) copy_n = body_avail;
        req->body = (char*)malloc(copy_n + 1);
        if (req->body) {
            if (copy_n > 0) memcpy(req->body, header_start, copy_n);
            req->body[copy_n] = '\0';
            req->body_length = copy_n;
        } else {
            req->body_length = 0;
        }
    } else if (header_start && *header_start) {
        req->body = strdup(header_start);
        req->body_length = req->body ? strlen(req->body) : 0;
    } else {
        req->body = NULL;
        req->body_length = 0;
    }
    
    req->param_keys = NULL;
    req->param_values = NULL;
    req->param_count = 0;

    return req;
}

HttpRequest* http_parse_request(const char* raw_request) {
    if (!raw_request) return NULL;
    return http_parse_request_n(raw_request, strlen(raw_request));
}

const char* http_get_header(HttpRequest* req, const char* key) {
    if (!req || !key || !req->header_keys || !req->header_values) return NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (!req->header_keys[i]) continue;
        if (strcasecmp(req->header_keys[i], key) == 0) {
            return req->header_values[i];
        }
    }
    return NULL;
}

// Parse req->query_string into req->query_keys / query_values once and
// cache on the request. Called lazily from http_get_query_param so any
// request that never asks for a query param pays nothing.
//
// The previous implementation walked the raw query_string on every call
// with strstr and returned a pointer into a function-local static buffer
// (#625): two sequential calls overwrote the buffer, so the first call's
// returned pointer aliased the second call's value. With a real per-key
// array each call returns a stable per-request pointer.
static void http_parse_query_locked(HttpRequest* req) {
    req->query_parsed = 1;
    if (!req->query_string || !*req->query_string) return;

    // First pass: count pairs (one per '&' boundary, plus the tail).
    int cap = 1;
    for (const char* p = req->query_string; *p; p++) {
        if (*p == '&') cap++;
    }
    req->query_keys   = (char**)calloc((size_t)cap, sizeof(char*));
    req->query_values = (char**)calloc((size_t)cap, sizeof(char*));
    if (!req->query_keys || !req->query_values) {
        free(req->query_keys);   req->query_keys = NULL;
        free(req->query_values); req->query_values = NULL;
        return;
    }

    const char* cur = req->query_string;
    while (*cur) {
        const char* amp = strchr(cur, '&');
        size_t pair_len = amp ? (size_t)(amp - cur) : strlen(cur);
        const char* eq = (const char*)memchr(cur, '=', pair_len);
        if (pair_len > 0 && eq && eq > cur) {
            size_t klen = (size_t)(eq - cur);
            size_t vlen = pair_len - klen - 1;
            char* k = (char*)malloc(klen + 1);
            char* v = (char*)malloc(vlen + 1);
            if (k && v) {
                memcpy(k, cur, klen); k[klen] = '\0';
                memcpy(v, eq + 1, vlen); v[vlen] = '\0';
                req->query_keys[req->query_count]   = k;
                req->query_values[req->query_count] = v;
                req->query_count++;
            } else {
                free(k); free(v);
            }
        }
        // Bare-key form (`?foo&bar=1`) is silently skipped — matches the
        // previous behaviour where strchr('=') would return NULL on it.
        if (!amp) break;
        cur = amp + 1;
    }
}

const char* http_get_query_param(HttpRequest* req, const char* key) {
    if (!req || !key) return NULL;
    if (!req->query_parsed) http_parse_query_locked(req);
    for (int i = 0; i < req->query_count; i++) {
        if (strcmp(req->query_keys[i], key) == 0) {
            return req->query_values[i];
        }
    }
    return NULL;
}

const char* http_get_path_param(HttpRequest* req, const char* key) {
    for (int i = 0; i < req->param_count; i++) {
        if (strcmp(req->param_keys[i], key) == 0) {
            return req->param_values[i];
        }
    }
    return NULL;
}

void http_request_free(HttpRequest* req) {
    if (!req) return;
    
    free(req->method);
    free(req->path);
    free(req->query_string);
    free(req->http_version);
    free(req->body);
    
    for (int i = 0; i < req->header_count; i++) {
        free(req->header_keys[i]);
        free(req->header_values[i]);
    }
    free(req->header_keys);
    free(req->header_values);
    
    for (int i = 0; i < req->param_count; i++) {
        free(req->param_keys[i]);
        free(req->param_values[i]);
    }
    free(req->param_keys);
    free(req->param_values);

    for (int i = 0; i < req->query_count; i++) {
        free(req->query_keys[i]);
        free(req->query_values[i]);
    }
    free(req->query_keys);
    free(req->query_values);

    free(req->remote_addr);
    free(req->local_addr);

    free(req);
}

// Response building
HttpServerResponse* http_response_create() {
    HttpServerResponse* res = (HttpServerResponse*)calloc(1, sizeof(HttpServerResponse));
    if (!res) return NULL;
    res->status_code = 200;
    res->status_text = strdup("OK");
    res->header_keys = (char**)malloc(sizeof(char*) * 50);
    res->header_values = (char**)malloc(sizeof(char*) * 50);
    res->header_count = 0;
    res->body = NULL;
    res->body_length = 0;
    /* No sendfile path staged by default. */
    res->sendfile_fd = -1;
    res->sendfile_size = 0;
    res->takeover_conn = NULL;
    res->takeover_taken = 0;

    // Add default headers
    http_response_set_header(res, "Content-Type", "text/html; charset=utf-8");
    http_response_set_header(res, "Server", "Aether/1.0");

    return res;
}

void http_response_set_status(HttpServerResponse* res, int code) {
    if (!res) return;
    res->status_code = code;
    free(res->status_text);
    res->status_text = strdup(http_status_text(code));
}

void http_response_set_header(HttpServerResponse* res, const char* key, const char* value) {
    if (!res) return;
    /* A header carrying a line ending would be written into the response head
     * verbatim and read back by the client as headers of its own, and a
     * doubled one ends the head and starts a second response (CWE-113). An
     * application that reflects anything a user supplied into a header is the
     * ordinary way this happens. The header is dropped rather than repaired:
     * this returns void, and emitting something other than what was asked for
     * is worse than emitting nothing. */
    if (!http_header_name_ok(key) || !http_header_value_ok(value)) return;

    // Lazy-allocate the header arrays. http_response_create() sets them
    // up eagerly, but external callers constructing the response struct
    // themselves (e.g. a C dispatch layer that wants to hand a response
    // into Aether handlers) only zero the struct. Without this, the
    // strdup-into-NULL below was a crash on the first header write.
    if (!res->header_keys || !res->header_values) {
        res->header_keys = (char**)calloc(50, sizeof(char*));
        res->header_values = (char**)calloc(50, sizeof(char*));
        res->header_count = 0;
        if (!res->header_keys || !res->header_values) return;
    }

    // Check if header exists, update it
    for (int i = 0; i < res->header_count; i++) {
        if (strcasecmp(res->header_keys[i], key) == 0) {
            char* vd = strdup(value);
            if (!vd) return;  // keep the existing value rather than free-to-NULL
            free(res->header_values[i]);
            res->header_values[i] = vd;
            return;
        }
    }

    // Add new header (max 50). Store only if both copies succeed; a NULL key
    // would crash the strcasecmp above on the next call, a NULL value the
    // serializer's strlen.
    if (res->header_count >= 50) return;
    char* kd = strdup(key);
    char* vd = strdup(value);
    if (!kd || !vd) { free(kd); free(vd); return; }
    res->header_keys[res->header_count] = kd;
    res->header_values[res->header_count] = vd;
    res->header_count++;
}

// Drop every header currently set on the response, including the
// defaults (`Content-Type: text/html; charset=utf-8` and `Server:
// Aether/1.0`) that http_response_create injects. Used by the VCR
// dispatcher to reset the response to a blank slate before emitting
// the tape's recorded headers verbatim — record/replay must serve
// exactly what was captured, not Aether's defaults overlaid on top.
//
// Leaves the header_keys/header_values arrays allocated so subsequent
// set_header / add_header calls don't have to re-malloc.
void http_response_clear_headers(HttpServerResponse* res) {
    if (!res) return;
    if (!res->header_keys || !res->header_values) return;
    for (int i = 0; i < res->header_count; i++) {
        free(res->header_keys[i]);
        free(res->header_values[i]);
        res->header_keys[i] = NULL;
        res->header_values[i] = NULL;
    }
    res->header_count = 0;
}

// Append a header value verbatim, even if a header with the same name
// already exists. Use this for protocols that allow (or require)
// repeated headers — e.g. SVN's WebDAV uses 13+ `DAV:` response
// headers, each with a distinct value. The replace-on-duplicate
// behaviour of `set_header` collapses these to the last value, which
// breaks SVN clients. `add_header` preserves the wire order and
// multiplicity that the serializer at line 1149 emits as separate
// `Name: Value\r\n` lines.
//
// 50-header cap is shared with `set_header`; over-cap silently drops.
void http_response_add_header(HttpServerResponse* res, const char* key, const char* value) {
    if (!res) return;
    /* Same rejection as set_header: a line ending here splits the response. */
    if (!http_header_name_ok(key) || !http_header_value_ok(value)) return;
    if (!res->header_keys || !res->header_values) {
        res->header_keys = (char**)calloc(50, sizeof(char*));
        res->header_values = (char**)calloc(50, sizeof(char*));
        res->header_count = 0;
        if (!res->header_keys || !res->header_values) return;
    }
    if (res->header_count >= 50) return;
    char* kd = strdup(key);
    char* vd = strdup(value);
    if (!kd || !vd) { free(kd); free(vd); return; }
    res->header_keys[res->header_count] = kd;
    res->header_values[res->header_count] = vd;
    res->header_count++;
}

void http_response_set_body(HttpServerResponse* res, const char* body) {
    if (!res || !body) return;
    free(res->body);
    res->body = strdup(body);
    res->body_length = strlen(body);

    // Update Content-Length
    char len_str[32];
    snprintf(len_str, sizeof(len_str), "%zu", res->body_length);
    http_response_set_header(res, "Content-Length", len_str);
}

/* Length-aware sibling — binary-safe set_body. The plain set_body
 * above uses strdup + strlen, so any embedded NUL truncates the
 * payload and the wire body comes out short. Reach for this when
 * the body is binary content (gzip / image / packed binary) or
 * may otherwise contain NUL bytes mid-payload.
 *
 * `body` is treated as `length` bytes verbatim — no NUL termination
 * required from the caller, no NUL searching done internally. The
 * stored buffer is one byte longer than `length` and NUL-terminated
 * so any code path that reads it as a C string still sees a valid
 * pointer (it just won't see the bytes after the first NUL via
 * strlen). Issue: see svn-aether's svnserver_respond_binary_ok
 * shim, which exists only because this function didn't.
 *
 * `length < 0` is treated as a no-op. `length == 0` clears the body.
 * `body == NULL` with `length > 0` is a no-op (defensive — same as
 * how set_body rejects NULL body). */
void http_response_set_body_n(HttpServerResponse* res, const char* body, int length) {
    if (!res) return;
    if (length < 0) return;
    if (length > 0 && !body) return;

    /* Cap-aware (#343): res->body's allocation size is stored in
     * res->body_length (we always alloc length+1 but the +1 NUL
     * terminator is included in cap accounting too — track the
     * full allocation in res->body_cap so caps_free recovers the
     * exact byte count regardless of how the body was set). */
    aether_caps_free(res->body, res->body_cap);
    if (length == 0) {
        res->body = NULL;
        res->body_length = 0;
        res->body_cap = 0;
    } else {
        const char* src = body;
        if (is_aether_string(body)) {
            src = ((const AetherString*)body)->data;
        }
        size_t alloc_cap = (size_t)length + 1;
        res->body = (char*)aether_caps_malloc(alloc_cap);
        if (!res->body) {
            res->body_length = 0;
            res->body_cap = 0;
            return;
        }
        memcpy(res->body, src, (size_t)length);
        res->body[length] = '\0';
        res->body_length = (size_t)length;
        res->body_cap = alloc_cap;
    }

    char len_str[32];
    snprintf(len_str, sizeof(len_str), "%zu", res->body_length);
    http_response_set_header(res, "Content-Length", len_str);
}

void http_response_json(HttpServerResponse* res, const char* json) {
    http_response_set_header(res, "Content-Type", "application/json");
    http_response_set_body(res, json);
}

void* http_response_accept_tunnel(HttpServerResponse* res) {
    if (!res || res->takeover_taken || !res->takeover_conn) return NULL;

    HttpConn* conn = (HttpConn*)res->takeover_conn;
    if (!conn || conn->fd < 0) return NULL;

#ifdef AETHER_HAS_OPENSSL
    /* std.tcp operates on raw sockets; a TLS-wrapped HTTP connection
     * needs a different stream handle that preserves SSL_read/write. */
    if (conn->ssl) return NULL;
#endif

    /* A raw TcpSocket cannot replay bytes the HTTP parser has already
     * read into HttpConn's keep-alive buffer. Refuse rather than drop
     * early tunnel bytes. Normal CONNECT clients wait for the 200
     * response before sending tunnel payload, so this is not hit in
     * the intended flow. */
    if (conn->write_pos > conn->read_pos) return NULL;

    size_t resp_len = 0;
    char* response_str = http_response_serialize_len(res, &resp_len);
    if (!response_str) return NULL;
    int sent = conn_send(conn, response_str, (int)resp_len);
    free(response_str);
    if (sent != (int)resp_len) return NULL;

    int fd = conn->fd;
    conn->fd = -1;
    res->takeover_taken = 1;
    res->takeover_conn = NULL;

    void* sock = tcp_socket_from_fd_owned(fd);
    if (!sock) {
        /* The fd has been closed by the adoption helper on failure;
         * suppress the normal response path because we already sent
         * the accepting response head. */
        return NULL;
    }
    return sock;
}

// Length-aware serializer. The body may be binary (gzip-compressed,
// other application/octet-stream payloads); the returned buffer is
// NOT a C string — caller must use *out_len, never strlen.
char* http_response_serialize_len(HttpServerResponse* res, size_t* out_len) {
    if (!res) { if (out_len) *out_len = 0; return NULL; }
    // Compute required size: status line + headers + blank line + body
    size_t needed = 64;  // status line headroom
    for (int i = 0; i < res->header_count; i++)
        needed += strlen(res->header_keys[i]) + strlen(res->header_values[i]) + 4;
    needed += 2;  // blank line
    if (res->body) needed += res->body_length;

    char* buf = malloc(needed + 1);
    if (!buf) { if (out_len) *out_len = 0; return NULL; }

    int off = snprintf(buf, needed + 1, "HTTP/1.1 %d %s\r\n",
                       res->status_code, res->status_text);
    for (int i = 0; i < res->header_count; i++)
        off += snprintf(buf + off, needed + 1 - off, "%s: %s\r\n",
                        res->header_keys[i], res->header_values[i]);
    off += snprintf(buf + off, needed + 1 - off, "\r\n");
    if (res->body && res->body_length > 0) {
        memcpy(buf + off, res->body, res->body_length);
        off += (int)res->body_length;
    }
    if (out_len) *out_len = (size_t)off;
    return buf;
}

// String-shaped legacy serializer. Equivalent to the length-aware
// variant for text bodies; truncates at the first NUL in the body
// for binary responses (callers wanting binary support should use
// http_response_serialize_len directly). Kept for backward compat
// with downstream consumers.
char* http_response_serialize(HttpServerResponse* res) {
    size_t len = 0;
    return http_response_serialize_len(res, &len);
}

void http_server_response_free(HttpServerResponse* res) {
    if (!res) return;

    free(res->status_text);
    /* Cap-aware (#343): body was alloc'd via aether_caps_malloc
     * (see http_response_set_body_n / sendfile-fallback read). */
    aether_caps_free(res->body, res->body_cap);

    for (int i = 0; i < res->header_count; i++) {
        free(res->header_keys[i]);
        free(res->header_values[i]);
    }
    free(res->header_keys);
    free(res->header_values);

    /* Issue #383: close any sendfile FD still owned by the response.
     * The connection writer normally closes it when the fast path
     * completes, but we close again here defensively for the
     * ineligible-fallback or error paths where the fd may still be
     * open. -1 indicates "already closed or never opened". */
    if (res->sendfile_fd >= 0) {
        close(res->sendfile_fd);
        res->sendfile_fd = -1;
    }

    free(res);
}

const char* http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

// Routing
void http_server_add_route(HttpServer* server, const char* method, const char* path, HttpHandler handler, void* user_data) {
    HttpRoute* route = (HttpRoute*)malloc(sizeof(HttpRoute));
    if (!route) return;
    route->method = strdup(method);
    route->path_pattern = strdup(path);
    // A NULL method/path_pattern would crash route matching's strcmp; drop the
    // half-built route rather than register it.
    if (!route->method || !route->path_pattern) {
        free(route->method);
        free(route->path_pattern);
        free(route);
        return;
    }
    route->handler = handler;
    route->user_data = user_data;
    route->next = server->routes;
    server->routes = route;
}

void http_server_get(HttpServer* server, const char* path, HttpHandler handler, void* user_data) {
    http_server_add_route(server, "GET", path, handler, user_data);
}

void http_server_post(HttpServer* server, const char* path, HttpHandler handler, void* user_data) {
    http_server_add_route(server, "POST", path, handler, user_data);
}

void http_server_put(HttpServer* server, const char* path, HttpHandler handler, void* user_data) {
    http_server_add_route(server, "PUT", path, handler, user_data);
}

void http_server_delete(HttpServer* server, const char* path, HttpHandler handler, void* user_data) {
    http_server_add_route(server, "DELETE", path, handler, user_data);
}

void http_server_use_middleware(HttpServer* server, HttpMiddleware middleware, void* user_data) {
    HttpMiddlewareNode* node = (HttpMiddlewareNode*)malloc(sizeof(HttpMiddlewareNode));
    if (!node) return;
    node->middleware = middleware;
    node->user_data = user_data;
    node->next = NULL;
    /* Append to the end of the chain so registration order matches
     * execution order — what every HTTP framework does, what the
     * std.http.middleware factories assume, and what the
     * documentation will say. (The earlier prepend-to-head shape
     * silently reversed the order, which only matters when more
     * than one middleware is installed; #260 Tier 1 is the first
     * caller that ever installs more than one.) */
    if (!server->middleware_chain) {
        server->middleware_chain = node;
    } else {
        HttpMiddlewareNode* tail = server->middleware_chain;
        while (tail->next) tail = tail->next;
        tail->next = node;
    }
}

/* Response-transformer chain node. Hidden in .c — header only
 * forward-declares the struct via the field's `struct
 * HttpResponseTransformerNode*` reference so the type is stable
 * across translation units. */
struct HttpResponseTransformerNode {
    HttpResponseTransformer xform;
    void* user_data;
    struct HttpResponseTransformerNode* next;
};

/* Per-request observation hook chain node (#260 Tier 3 F1/F2). */
struct HttpRequestHookNode {
    HttpRequestHook hook;
    void* user_data;
    struct HttpRequestHookNode* next;
};

/* SSE route node (#260 Tier 2). */
struct HttpSseRoute {
    char* path;
    HttpSseHandler handler;
    void* user_data;
    struct HttpSseRoute* next;
};

/* Public SSE-connection handle exposed to user handlers. Wraps the
 * underlying HttpConn so http_sse_send_event can write directly to
 * the wire (with TLS unwrap when applicable, via conn_send). */
struct HttpSseConn {
    HttpConn* conn;     /* not owned */
    int       closed;
};

/* WebSocket route node (#260 Tier 2 / E2). */
struct HttpWsRoute {
    char* path;
    HttpWsHandler handler;
    void* user_data;
    struct HttpWsRoute* next;
};

/* Public WebSocket-connection handle. Wraps the HttpConn plus a
 * per-connection recv buffer for assembled message reassembly. */
struct HttpWsConn {
    HttpConn* conn;       /* not owned */
    int       closed;
    /* Reassembled message buffer — grown as needed. */
    char*  msg_buf;
    int    msg_cap;
    int    msg_len;
    /* For binary frames we don't NUL-terminate, so callers see
     * (out_data, out_len). For text frames we NUL-terminate for
     * convenience. The opcode of the in-progress message is
     * carried across continuation frames. */
    int    msg_opcode;    /* 0x1 text, 0x2 binary */
};

void http_server_websocket(HttpServer* server, const char* path,
                           HttpWsHandler handler, void* user_data) {
    if (!server || !path || !handler) return;
    struct HttpWsRoute* r = (struct HttpWsRoute*)calloc(1, sizeof(*r));
    if (!r) return;
    r->path = strdup(path);
    r->handler = handler;
    r->user_data = user_data;
    r->next = server->ws_routes;
    server->ws_routes = r;
}

void http_server_sse(HttpServer* server, const char* path,
                     HttpSseHandler handler, void* user_data) {
    if (!server || !path || !handler) return;
    struct HttpSseRoute* r = (struct HttpSseRoute*)calloc(1, sizeof(*r));
    if (!r) return;
    r->path = strdup(path);
    r->handler = handler;
    r->user_data = user_data;
    r->next = server->sse_routes;
    server->sse_routes = r;
}

int http_sse_send_event(HttpSseConn* sse,
                        const char* event_name,
                        const char* data) {
    return http_sse_send_event_id(sse, event_name, data, NULL);
}

int http_sse_send_event_id(HttpSseConn* sse,
                           const char* event_name,
                           const char* data,
                           const char* id) {
    if (!sse || !sse->conn || sse->closed) return -1;

    /* Build an SSE chunk:
     *     id: <id>\n           (optional)
     *     event: <name>\n      (optional)
     *     data: <line1>\n
     *     data: <line2>\n      (one line per \n in data)
     *     \n                   (terminator)
     */
    /* Worst case: data is N bytes, every byte is \n -> N "data: \n"
     * lines plus the constant overhead. Round generously to 4*N + 256. */
    size_t data_len = data ? strlen(data) : 0;
    /* Cap-aware (#343): `cap` is driven by the handler-supplied event
     * payload (`data`/`event_name`/`id`), so an SSE-emitting plugin
     * can size this allocation. The buffer is built and freed wholly
     * inside this function — `cap` is in scope at the matching free
     * below — so it threads cleanly through the caps allocator. */
    size_t cap = data_len * 4 + 256
               + (event_name ? strlen(event_name) + 16 : 0)
               + (id ? strlen(id) + 16 : 0);
    char* buf = (char*)aether_caps_malloc(cap);
    if (!buf) return -1;
    size_t off = 0;

    if (id && *id) {
        off += (size_t)snprintf(buf + off, cap - off, "id: %s\n", id);
    }
    if (event_name && *event_name) {
        off += (size_t)snprintf(buf + off, cap - off, "event: %s\n", event_name);
    }
    if (data && *data) {
        const char* line_start = data;
        const char* p = data;
        for (;;) {
            if (*p == '\n' || *p == '\0') {
                size_t line_len = (size_t)(p - line_start);
                if (off + 7 + line_len + 1 >= cap) break;
                memcpy(buf + off, "data: ", 6); off += 6;
                memcpy(buf + off, line_start, line_len); off += line_len;
                buf[off++] = '\n';
                if (*p == '\0') break;
                line_start = p + 1;
            }
            p++;
        }
    } else {
        if (off + 8 < cap) {
            memcpy(buf + off, "data: \n", 7);
            off += 7;
        }
    }
    if (off + 2 < cap) {
        buf[off++] = '\n';   /* event terminator */
    }

    int n = conn_send(sse->conn, buf, (int)off);
    aether_caps_free(buf, cap);
    if (n < (int)off) {
        sse->closed = 1;
        return -1;
    }
    return 0;
}

void http_sse_close(HttpSseConn* sse) {
    if (!sse) return;
    sse->closed = 1;
}

// =================================================================
// WebSocket framing (RFC 6455) — #260 Tier 2 / E2
// =================================================================
//
// Frame format (server-side perspective):
//   Byte 0:  FIN (1) | RSV1-3 (3) | opcode (4)
//   Byte 1:  MASK (1) | payload-length (7)
//   Bytes 2-9 (variable): extended payload-length + masking-key (4 bytes if MASK=1)
//   Bytes N+:            payload (XORed with mask if MASK=1)
//
// Client-to-server frames are always masked; server-to-client are
// always unmasked. We honor that asymmetry on both sides.

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* Read exactly N bytes from conn into buf, draining any pre-buffered
 * bytes in conn->buf first (these accumulate when handle_one_request's
 * recv pulled past the request's header boundary — for WebSocket
 * upgrades, the client may have sent the first WS frame in the same
 * TCP packet as the upgrade headers). Returns 0 on success, -1 on
 * EOF / error. */
static int ws_recv_exact(HttpConn* conn, void* buf, int n) {
    char* p = (char*)buf;
    /* Drain conn->buf first. */
    int avail = conn->write_pos - conn->read_pos;
    if (avail > 0) {
        int take = (avail >= n) ? n : avail;
        memcpy(p, conn->buf + conn->read_pos, (size_t)take);
        conn->read_pos += take;
        p += take;
        n -= take;
    }
    /* Fall through to socket recv for the remainder. */
    while (n > 0) {
        int got = conn_recv(conn, p, n);
        if (got <= 0) return -1;
        p += got;
        n -= got;
    }
    return 0;
}

/* Send a server-to-client frame (unmasked). opcode + payload bytes.
 * Returns 0 on success, -1 on transport error. */
static int ws_send_frame(HttpConn* conn, int opcode,
                         const void* payload, int payload_len) {
    unsigned char hdr[10];
    int hlen = 0;
    hdr[hlen++] = (unsigned char)(0x80 | (opcode & 0x0F));  /* FIN=1 */
    if (payload_len < 126) {
        hdr[hlen++] = (unsigned char)payload_len;
    } else if (payload_len <= 0xFFFF) {
        hdr[hlen++] = 126;
        hdr[hlen++] = (unsigned char)((payload_len >> 8) & 0xFF);
        hdr[hlen++] = (unsigned char)(payload_len & 0xFF);
    } else {
        hdr[hlen++] = 127;
        unsigned long long pl = (unsigned long long)payload_len;
        hdr[hlen++] = (unsigned char)((pl >> 56) & 0xFF);
        hdr[hlen++] = (unsigned char)((pl >> 48) & 0xFF);
        hdr[hlen++] = (unsigned char)((pl >> 40) & 0xFF);
        hdr[hlen++] = (unsigned char)((pl >> 32) & 0xFF);
        hdr[hlen++] = (unsigned char)((pl >> 24) & 0xFF);
        hdr[hlen++] = (unsigned char)((pl >> 16) & 0xFF);
        hdr[hlen++] = (unsigned char)((pl >> 8) & 0xFF);
        hdr[hlen++] = (unsigned char)(pl & 0xFF);
    }
    if (conn_send(conn, hdr, hlen) != hlen) return -1;
    if (payload_len > 0) {
        if (conn_send(conn, payload, payload_len) != payload_len) return -1;
    }
    return 0;
}

int http_ws_send_text(HttpWsConn* ws, const char* text) {
    if (!ws || !ws->conn || ws->closed) return -1;
    int n = text ? (int)strlen(text) : 0;
    return ws_send_frame(ws->conn, WS_OP_TEXT, text ? text : "", n);
}

int http_ws_send_binary(HttpWsConn* ws, const void* data, int len) {
    if (!ws || !ws->conn || ws->closed) return -1;
    return ws_send_frame(ws->conn, WS_OP_BIN, data, len);
}

void http_ws_close(HttpWsConn* ws, int code, const char* reason) {
    if (!ws || ws->closed) return;
    /* Close payload: 2 bytes status code (network order) + UTF-8
     * reason. Reason capped at 123 bytes per spec. */
    unsigned char payload[125];
    payload[0] = (unsigned char)((code >> 8) & 0xFF);
    payload[1] = (unsigned char)(code & 0xFF);
    int rn = 0;
    if (reason && *reason) {
        rn = (int)strlen(reason);
        if (rn > 123) rn = 123;
        memcpy(payload + 2, reason, rn);
    }
    ws_send_frame(ws->conn, WS_OP_CLOSE, payload, 2 + rn);
    ws->closed = 1;
}

/* Grow the message-reassembly buffer if needed. */
static int ws_msg_grow(HttpWsConn* ws, int need) {
    if (ws->msg_cap >= need) return 0;
    int new_cap = ws->msg_cap > 0 ? ws->msg_cap : 4096;
    while (new_cap < need) new_cap *= 2;
    /* Cap-aware (#343): WebSocket message reassembly is driven by the
     * peer's frame sizes — untrusted. Old size is the prior
     * ws->msg_cap (0 on the first grow from NULL); the matching free
     * at the end of the WS dispatch passes the same msg_cap. */
    char* nb = (char*)aether_caps_realloc(ws->msg_buf, (size_t)ws->msg_cap,
                                          (size_t)new_cap);
    if (!nb) return -1;
    ws->msg_buf = nb;
    ws->msg_cap = new_cap;
    return 0;
}

const char* http_ws_message_data(HttpWsConn* ws) {
    if (!ws || !ws->msg_buf) return "";
    return ws->msg_buf;
}

int http_ws_message_length(HttpWsConn* ws) {
    return ws ? ws->msg_len : 0;
}

int http_ws_recv(HttpWsConn* ws) {
    if (!ws || !ws->conn || ws->closed) return -1;
    ws->msg_len = 0;  /* reset for this message */

    for (;;) {
        unsigned char hdr2[2];
        if (ws_recv_exact(ws->conn, hdr2, 2) != 0) { ws->closed = 1; return -1; }
        int fin    = (hdr2[0] >> 7) & 1;
        int opcode = hdr2[0] & 0x0F;
        int masked = (hdr2[1] >> 7) & 1;
        long long pl = hdr2[1] & 0x7F;

        if (pl == 126) {
            unsigned char ext[2];
            if (ws_recv_exact(ws->conn, ext, 2) != 0) { ws->closed = 1; return -1; }
            pl = ((long long)ext[0] << 8) | ext[1];
        } else if (pl == 127) {
            unsigned char ext[8];
            if (ws_recv_exact(ws->conn, ext, 8) != 0) { ws->closed = 1; return -1; }
            pl = 0;
            for (int i = 0; i < 8; i++) pl = (pl << 8) | ext[i];
        }

        unsigned char mask_key[4] = {0};
        if (masked) {
            if (ws_recv_exact(ws->conn, mask_key, 4) != 0) { ws->closed = 1; return -1; }
        }

        /* Sanity bound — refuse 1GB+ frames. */
        if (pl < 0 || pl > (1LL << 30)) { ws->closed = 1; return -1; }

        /* Read payload into a scratch buffer, unmasking on the fly.
         * Cap-aware (#343): `pl` is the WebSocket payload length
         * field from the wire — untrusted by definition. The 1 GiB
         * sanity bound above stops the obvious DoS shape; the caps
         * allocator threads this through to the host's
         * aether_set_memory_cap budget. */
        char* frame_buf = NULL;
        size_t frame_cap = 0;
        if (pl > 0) {
            frame_cap = (size_t)pl;
            frame_buf = (char*)aether_caps_malloc(frame_cap);
            if (!frame_buf) { ws->closed = 1; return -1; }
            if (ws_recv_exact(ws->conn, frame_buf, (int)pl) != 0) {
                aether_caps_free(frame_buf, frame_cap); ws->closed = 1; return -1;
            }
            if (masked) {
                for (long long i = 0; i < pl; i++) {
                    frame_buf[i] ^= (char)mask_key[i & 3];
                }
            }
        }

        if (opcode == WS_OP_PING) {
            /* Respond with a pong carrying the same payload (RFC 6455
             * §5.5.3). Free our buffer afterwards; caller doesn't see
             * control frames. */
            ws_send_frame(ws->conn, WS_OP_PONG, frame_buf, (int)pl);
            aether_caps_free(frame_buf, frame_cap);
            continue;
        }
        if (opcode == WS_OP_PONG) {
            aether_caps_free(frame_buf, frame_cap);
            continue;  /* unsolicited pong — discard */
        }
        if (opcode == WS_OP_CLOSE) {
            /* Echo close frame back (RFC 6455 §5.5.1) and report
             * to caller. */
            ws_send_frame(ws->conn, WS_OP_CLOSE, frame_buf, (int)pl);
            aether_caps_free(frame_buf, frame_cap);
            ws->closed = 1;
            return -1;
        }

        /* Data frame (text / binary / continuation). Append to
         * the message buffer, then break out if FIN. */
        if (opcode == WS_OP_TEXT || opcode == WS_OP_BIN) {
            ws->msg_opcode = opcode;
            ws->msg_len = 0;  /* fresh message */
        }
        if (pl > 0) {
            if (ws_msg_grow(ws, ws->msg_len + (int)pl + 1) != 0) {
                aether_caps_free(frame_buf, frame_cap); ws->closed = 1; return -1;
            }
            memcpy(ws->msg_buf + ws->msg_len, frame_buf, (size_t)pl);
            ws->msg_len += (int)pl;
        }
        aether_caps_free(frame_buf, frame_cap);

        if (fin) break;
    }

    /* NUL-terminate text messages for caller convenience; binary
     * messages get the explicit length only. */
    if (ws->msg_opcode == WS_OP_TEXT) {
        if (ws_msg_grow(ws, ws->msg_len + 1) != 0) { ws->closed = 1; return -1; }
        ws->msg_buf[ws->msg_len] = '\0';
    }
    return ws->msg_opcode == WS_OP_TEXT ? 1 : 2;
}

/* RFC 6455 handshake. Server responds 101 Switching Protocols with
 * Sec-WebSocket-Accept = Base64(SHA-1(client_key + magic-uuid)). We
 * call OpenSSL directly here for the raw 20-byte SHA-1 (the
 * std.cryptography wrappers expose hex output, not raw bytes —
 * adding raw to that surface is a separate cleanup). std-side
 * Base64 is fine as-is (raw bytes in, base64 string out). */
#ifdef AETHER_HAS_OPENSSL
#include <openssl/sha.h>
extern char* cryptography_base64_encode_raw(const char* data, int length);
#endif

static int ws_send_handshake(HttpConn* conn, const char* client_key) {
#ifdef AETHER_HAS_OPENSSL
    /* Magic GUID per RFC 6455 §1.3. */
    static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[256];
    int n = snprintf(concat, sizeof(concat), "%s%s", client_key, GUID);
    if (n <= 0 || n >= (int)sizeof(concat)) return -1;

    unsigned char digest[SHA_DIGEST_LENGTH];  /* 20 bytes */
    SHA1((const unsigned char*)concat, (size_t)n, digest);

    char* accept_b64 = cryptography_base64_encode_raw((const char*)digest,
                                                       SHA_DIGEST_LENGTH);
    if (!accept_b64) return -1;

    /* SHA-1 is 20 bytes -> 28 base64 chars + 1 padding '=' to reach
     * a multiple of 4. The cryptography wrapper is documented as
     * unpadded; append it ourselves. */
    char resp[512];
    int rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s=\r\n"
        "\r\n",
        accept_b64);
    free(accept_b64);
    if (rn <= 0 || rn >= (int)sizeof(resp)) return -1;
    if (conn_send(conn, resp, rn) != rn) return -1;
    return 0;
#else
    (void)conn; (void)client_key;
    return -1;
#endif
}

void http_server_use_request_hook(HttpServer* server,
                                  HttpRequestHook hook,
                                  void* user_data) {
    if (!server || !hook) return;
    struct HttpRequestHookNode* node =
        (struct HttpRequestHookNode*)malloc(sizeof(*node));
    node->hook = hook;
    node->user_data = user_data;
    node->next = NULL;
    if (!server->request_hook_chain) {
        server->request_hook_chain = node;
    } else {
        struct HttpRequestHookNode* tail = server->request_hook_chain;
        while (tail->next) tail = tail->next;
        tail->next = node;
    }
}

void http_server_use_response_transformer(HttpServer* server,
                                          HttpResponseTransformer xform,
                                          void* user_data) {
    if (!server || !xform) return;
    struct HttpResponseTransformerNode* node =
        (struct HttpResponseTransformerNode*)malloc(sizeof(*node));
    node->xform = xform;
    node->user_data = user_data;
    node->next = NULL;
    if (!server->response_transformer_chain) {
        server->response_transformer_chain = node;
    } else {
        struct HttpResponseTransformerNode* tail = server->response_transformer_chain;
        while (tail->next) tail = tail->next;
        tail->next = node;
    }
}

// Route matching with parameter extraction
int http_route_matches(const char* pattern, const char* path, HttpRequest* req) {
    // Free any params captured by a previous candidate-route check on this
    // request. A route table tries each pattern in turn until one matches, so
    // without this every non-matching (and every partially-matching) route
    // leaks the two arrays plus their strings, since http_request_free only
    // reclaims the final call's set. Doing it before the exact-match return
    // also clears stale params a failed earlier pattern left behind.
    if (req->param_keys) {
        for (int i = 0; i < req->param_count; i++) free(req->param_keys[i]);
        free(req->param_keys);
        req->param_keys = NULL;
    }
    if (req->param_values) {
        for (int i = 0; i < req->param_count; i++) free(req->param_values[i]);
        free(req->param_values);
        req->param_values = NULL;
    }
    req->param_count = 0;

    // Exact match
    if (strcmp(pattern, path) == 0) {
        return 1;
    }

    // Pattern matching with parameters
    const char* p = pattern;
    const char* u = path;

    // Allocate space for params
    req->param_keys = (char**)malloc(sizeof(char*) * 10);
    req->param_values = (char**)malloc(sizeof(char*) * 10);
    req->param_count = 0;
    if (!req->param_keys || !req->param_values) {
        free(req->param_keys);   req->param_keys = NULL;
        free(req->param_values); req->param_values = NULL;
        return 0;
    }

    while (*p && *u) {
        if (*p == ':') {
            // Parameter segment
            p++; // Skip ':'

            // Extract parameter name
            const char* param_start = p;
            while (*p && *p != '/') p++;

            int param_name_len = p - param_start;

            // Extract parameter value from URL
            const char* value_start = u;
            while (*u && *u != '/') u++;

            int value_len = u - value_start;

            // The arrays hold at most 10 params; a pattern with more would
            // overflow them. Treat that as a non-match rather than corrupt heap.
            if (req->param_count >= 10) return 0;

            char* param_name = (char*)malloc(param_name_len + 1);
            char* value = (char*)malloc(value_len + 1);
            if (!param_name || !value) { free(param_name); free(value); return 0; }
            strncpy(param_name, param_start, param_name_len);
            param_name[param_name_len] = '\0';
            strncpy(value, value_start, value_len);
            value[value_len] = '\0';

            req->param_keys[req->param_count] = param_name;
            req->param_values[req->param_count] = value;
            req->param_count++;

        } else if (*p == '*') {
            // Wildcard - matches anything remaining
            return 1;
        } else if (*p == *u) {
            p++;
            u++;
        } else {
            // No match
            return 0;
        }
    }
    
    // Both should be at end for exact match
    return (*p == '\0' && *u == '\0');
}

// Handle a single client connection
#include <time.h>

/* Monotonic microsecond clock for per-request latency measurement. */
static long http_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

/* HTTP/1.1 Connection-header inspection. Returns 1 if the client
 * asked to keep the connection open after this request, 0 if it asked
 * for close (explicitly or by HTTP/1.0 default). Used by the
 * keep-alive loop in handle_client_connection. */
static int http_request_wants_keepalive(HttpRequest* req) {
    if (!req) return 0;
    const char* conn_hdr = http_get_header(req, "Connection");
    int is_http_1_0 = req->http_version &&
                      strstr(req->http_version, "HTTP/1.0") != NULL;
    if (conn_hdr) {
        if (http_strcasestr(conn_hdr, "close") != NULL) return 0;
        if (http_strcasestr(conn_hdr, "keep-alive") != NULL) return 1;
    }
    /* No header: HTTP/1.1 defaults to keep-alive, HTTP/1.0 to close. */
    return is_http_1_0 ? 0 : 1;
}

/* Route lookup, shared by both dispatch paths. HEAD is GET-without-body
 * (RFC 9110 section 9.3.2), so a path with only a GET route answers HEAD with
 * the shape GET would have produced; an exact HEAD route still wins. The
 * keep-alive path had no such fallback and answered every HEAD with 404. */
static HttpRoute* http_resolve_route(HttpServer* server, HttpRequest* req) {
    HttpRoute* head_to_get = NULL;
    int is_head = req->method && strcmp(req->method, "HEAD") == 0;
    for (HttpRoute* route = server->routes; route; route = route->next) {
        if (!http_route_matches(route->path_pattern, req->path, req)) continue;
        if (strcmp(route->method, req->method) == 0 ||
            strcmp(route->method, "*") == 0) {
            return route;
        }
        if (is_head && !head_to_get && strcmp(route->method, "GET") == 0) {
            head_to_get = route;
        }
    }
    return head_to_get;
}

/* Per-request slice of the connection lifecycle. Returns:
 *    1 — request was processed; caller may loop for another request
 *        on the same connection (subject to keep-alive policy).
 *    0 — connection should close (parse failure, EOF, client asked
 *        for close, or the response status mandates close).
 *
 * Uses the conn's persistent read buffer so that bytes the previous
 * request's recv loop pulled past its own boundary (HTTP pipelining,
 * or just two requests in one TCP packet) are not dropped. */
/* Emit `res` on `conn` and decide whether the connection carries another
 * request. Owns `req` and `res` and frees both. Reached from the route
 * handler AND from a middleware short-circuit: the short-circuit used to
 * send the response itself and close, so every reverse-proxied response
 * (the proxy middleware answers by short-circuiting) closed its inbound
 * connection, whatever the server keep-alive setting said (#1653). */
static int finish_response(HttpServer* server, HttpConn* conn,
                           HttpRequest* req, HttpServerResponse* res,
                           int requests_served, int max_requests) {
    /* Decide whether this response keeps the connection open.
     * Three things can force close:
     *   - keep-alive disabled on the server,
     *   - client requested close (or HTTP/1.0 default),
     *   - max_requests reached (caller passes the post-increment).
     * We also force close on response statuses that semantically
     * mandate it (408 Request Timeout, 426 Upgrade Required). */
    int will_keep_alive = server->keep_alive_enabled
                       && http_request_wants_keepalive(req)
                       && (max_requests == 0 ||
                           requests_served + 1 < max_requests)
                       && res->status_code != 408
                       && res->status_code != 426;

    /* Without somewhere to park it, a kept connection holds its worker for
     * its whole life, so keeping one while others wait in the accept queue
     * takes their turn rather than saving a handshake: at 20 concurrent
     * clients against 16 workers that measured 99 rps, because four
     * connections never reached a worker. #1653 shipped this rule, which
     * closes exactly when the pool is saturated.
     *
     * With a parking lot the premise is gone: an idle connection costs a
     * descriptor, not a thread, and closing it would throw away the handshake
     * for no one's benefit (#1663). Keeping this rule on made the server
     * close 284 of 300 idle keep-alive connections, which is the behaviour
     * parking exists to remove. */
    if (will_keep_alive && !server->park_lot && http_pool_pending() > 0) {
        will_keep_alive = 0;
    }

    /* A persistent connection needs a definite body length, or the client
     * cannot tell where this response ends and the next begins. Responses
     * built without a body carry no Content-Length (http_response_create
     * sets only Content-Type and Server), so state it rather than close. */
    if (will_keep_alive) {
        int has_length = 0;
        for (int i = 0; i < res->header_count; i++) {
            if (strcasecmp(res->header_keys[i], "Content-Length") == 0 ||
                strcasecmp(res->header_keys[i], "Transfer-Encoding") == 0) {
                has_length = 1;
                break;
            }
        }
        if (!has_length) {
            char len_str[32];
            long long len = res->sendfile_fd >= 0
                ? (long long)res->sendfile_size : (long long)res->body_length;
            snprintf(len_str, sizeof(len_str), "%lld", len);
            http_response_set_header(res, "Content-Length", len_str);
        }
    }

    /* Emit the right Connection / Keep-Alive headers so the client
     * knows what we're going to do. */
    if (will_keep_alive) {
        http_response_set_header(res, "Connection", "keep-alive");
        if (max_requests > 0) {
            char ka[64];
            int remaining = max_requests - requests_served - 1;
            int idle_sec = server->keep_alive_idle_ms > 0
                ? server->keep_alive_idle_ms / 1000 : 30;
            snprintf(ka, sizeof(ka), "timeout=%d, max=%d", idle_sec, remaining);
            http_response_set_header(res, "Keep-Alive", ka);
        }
    } else {
        http_response_set_header(res, "Connection", "close");
    }

    /* Issue #383 zero-copy: dispatched via the helper so the same
     * behaviour applies whether we got here from the route handler
     * or from a middleware short-circuit (the chain at line ~2270
     * also calls this helper). */
    int force_close = send_response_with_optional_sendfile(conn, res, req);
    if (force_close) will_keep_alive = 0;

    // Cleanup
    http_request_free(req);
    http_server_response_free(res);
    return will_keep_alive;
}

/* handle_one_request's third answer, alongside 1 (keep going) and 0 (close):
 * the connection is idle between requests and may be parked. Distinct from 0
 * so a broken connection is still closed rather than parked. */
#define HTTP_REQUEST_IDLE 2

/* Did the last recv fail because the socket's receive timeout fired, rather
 * than because the peer went away? */
static int conn_recv_timed_out(void) {
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* Resolve this connection's peer and local address, once (#1719).
 *
 * Idempotent: after the first call `addrs_resolved` is set and the cached
 * text is returned unchanged, including when the lookup failed and the
 * cache holds "". A failed lookup is a real answer for a Unix-domain
 * socket, and retrying it per request is exactly the cost being removed.
 *
 * Not thread-safe by design: an HttpConn is owned by one worker at a
 * time. Parking hands it between workers, but never concurrently. */
static void conn_resolve_addrs(HttpConn* c) {
    if (!c || c->addrs_resolved) return;
    c->addrs_resolved = 1;
    c->remote_addr[0] = '\0';
    c->local_addr[0] = '\0';
    c->remote_port = 0;
    c->local_port = 0;
    if (c->fd < 0) return;

    struct sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    if (getpeername(c->fd, (struct sockaddr*)&ss, &sslen) == 0) {
        const char* src = NULL;
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)&ss;
            src = inet_ntop(AF_INET, &sa->sin_addr,
                            c->remote_addr, sizeof(c->remote_addr));
            c->remote_port = ntohs(sa->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6* sa = (struct sockaddr_in6*)&ss;
            src = inet_ntop(AF_INET6, &sa->sin6_addr,
                            c->remote_addr, sizeof(c->remote_addr));
            c->remote_port = ntohs(sa->sin6_port);
        }
        if (!src) { c->remote_addr[0] = '\0'; c->remote_port = 0; }
    }

    sslen = sizeof(ss);
    if (getsockname(c->fd, (struct sockaddr*)&ss, &sslen) == 0) {
        const char* src = NULL;
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)&ss;
            src = inet_ntop(AF_INET, &sa->sin_addr,
                            c->local_addr, sizeof(c->local_addr));
            c->local_port = ntohs(sa->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6* sa = (struct sockaddr_in6*)&ss;
            src = inet_ntop(AF_INET6, &sa->sin6_addr,
                            c->local_addr, sizeof(c->local_addr));
            c->local_port = ntohs(sa->sin6_port);
        }
        if (!src) { c->local_addr[0] = '\0'; c->local_port = 0; }
    }
}

/* Is every header line in this block one this server will read the same way
 * as whoever sent it?
 *
 * Two shapes are rejected rather than interpreted, because interpreting them
 * is where recipients disagree and a disagreement about a framing header is
 * request smuggling:
 *
 *   "Content-Length : 5"  whitespace between the name and the colon. RFC 9112
 *                         5.1 requires a server to reject it. Ignoring it,
 *                         which is what an exact-match scan does, means this
 *                         server sees no body where a laxer front end sees one.
 *
 *   "X-Fold: a\r\n b"      an obs-fold continuation. RFC 9112 5.2 deprecated it
 *                         and requires a server that does not support it to
 *                         reject the message rather than guess at where the
 *                         value ends.
 */
static int conn_headers_well_formed(const char* block, const char* end) {
    for (const char* line = block; line && line < end; ) {
        const char* eol = (const char*)memchr(line, '\n', (size_t)(end - line));
        const char* line_end = eol ? eol : end;
        size_t line_len = (size_t)(line_end - line);
        if (line_len && line[line_len - 1] == '\r') line_len--;

        if (line_len >= HTTP_MAX_HEADER_LINE) return -1;    /* too long to hold */
        if (line_len > 0) {
            if (line[0] == ' ' || line[0] == '\t') return 0;   /* obs-fold */
            const char* colon = (const char*)memchr(line, ':', line_len);
            if (!colon) return 0;                              /* no field name */
            if (colon > line && (colon[-1] == ' ' || colon[-1] == '\t')) return 0;
            size_t name_len = (size_t)(colon - line);
            if (name_len == 0) return 0;
            char name[128];
            if (name_len >= sizeof(name)) return 0;
            memcpy(name, line, name_len);
            name[name_len] = '\0';
            if (!http_header_name_ok(name)) return 0;
        }
        if (!eol) break;
        line = eol + 1;
    }
    return 1;
}

/* Find a header by name across the request's header block, anchored to the
 * start of each line.
 *
 * Anchoring matters: a substring search over the whole block also matches
 * inside another header's value, so a request carrying
 * `X-Note: Content-Length: 9` would have had that read as its framing.
 *
 * Returns how many times the header appears, writes the first value into
 * `out`, and sets *differing when two of them disagree. A framing header that
 * appears twice with two answers has no single answer, which is the shape a
 * smuggling pair is built from.
 */
static int conn_find_header(const char* block, const char* end,
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

/* Content-Length is a plain count of digits. Anything else, a sign, a spelled
 * number, a value that will not fit, has no length in it, and RFC 9112 6.3
 * calls that unrecoverable rather than something to guess at. */
static int conn_parse_content_length(const char* text, long* out) {
    if (!text || !*text) return -1;
    long value = 0;
    for (const unsigned char* c = (const unsigned char*)text; *c; c++) {
        if (*c < '0' || *c > '9') return -1;
        if (value > (LONG_MAX - (*c - '0')) / 10) return -1;
        value = value * 10 + (*c - '0');
    }
    *out = value;
    return 0;
}

/* The largest chunked request body this server will buffer. A chunked body
 * declares no length in advance, so nothing else bounds it. */
#define HTTP_MAX_CHUNKED_BODY (8 * 1024 * 1024)

static void conn_send_headers_too_large(HttpConn* conn) {
    static const char msg[] =
        "HTTP/1.1 431 Request Header Fields Too Large\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "\r\n"
        "header line is too long\n";
    conn_send(conn, msg, (int)(sizeof(msg) - 1));
}

static void conn_send_uri_too_long(HttpConn* conn) {
    static const char msg[] =
        "HTTP/1.1 414 URI Too Long\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 26\r\n"
        "Connection: close\r\n"
        "\r\n"
        "request line is too long\r\n";
    conn_send(conn, msg, (int)(sizeof(msg) - 1));
}

static void conn_send_payload_too_large(HttpConn* conn) {
    static const char msg[] =
        "HTTP/1.1 413 Payload Too Large\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 34\r\n"
        "Connection: close\r\n"
        "\r\n"
        "request body exceeds server limit\n";
    conn_send(conn, msg, (int)(sizeof(msg) - 1));
}

/* Refuse a message whose framing this server will not guess at, and say so
 * before closing. A framing error cannot be answered on a kept-alive
 * connection, because where the next request starts is exactly what is in
 * doubt. */
static void conn_send_bad_framing(HttpConn* conn) {
    static const char msg[] =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 45\r\n"
        "Connection: close\r\n"
        "\r\n"
        "request framing is ambiguous or unsupported\r\n";
    conn_send(conn, msg, (int)(sizeof(msg) - 1));
}

static int handle_one_request(HttpServer* server, HttpConn* conn,
                              int requests_served, int max_requests) {
    long t_start = http_now_us();
    /* Lazy-allocate the read buffer on first use. */
    if (!conn->buf) {
        if (conn_buf_ensure(conn, HTTP_CONN_BUF_CAP) != 0) return 0;
        conn->read_pos = 0;
        conn->write_pos = 0;
    }
    /* Reclaim space at the head if the previous request consumed it. */
    conn_buf_compact(conn);

#ifdef AETHER_HAS_NGHTTP2
    /* Prior-knowledge h2 detection (RFC 7540 §3.5). When the server
     * has h2 enabled, the very first request on a fresh connection
     * may be the h2 connection preface ("PRI * HTTP/2.0\r\n\r\nSM…")
     * rather than an HTTP/1.1 request line. Probe before attempting
     * the HTTP/1.1 parse; on match, hand the connection to the h2
     * driver and return 0 so the outer keep-alive loop stops. */
    if (server->h2_enabled && requests_served == 0 && !conn->is_h2) {
        /* Buffer at least the 24-byte preface before deciding. The
         * existing read loop only reads when looking for \r\n\r\n;
         * pre-fetch a little here so the probe has bytes to inspect. */
        while ((conn->write_pos - conn->read_pos) < 24) {
            if (conn->write_pos + 1 >= conn->buf_cap) break;
            int n = conn_recv(conn, conn->buf + conn->write_pos,
                              conn->buf_cap - conn->write_pos - 1);
            if (n <= 0) return 0;  /* EOF / timeout / error */
            conn->write_pos += n;
        }
        int probe = conn_buffered_is_h2_preface(conn);
        if (probe == 1) {
            /* Bytes already consumed by the preface stay in the
             * conn buffer; handle_h2_connection feeds them to
             * nghttp2 before reading more from the wire. */
            conn->is_h2 = 1;
            handle_h2_connection(server, conn, NULL);
            return 0;
        }
        /* probe == 0 → continue as HTTP/1.1; probe == -1 was an
         * insufficient-data state that we already retried via the
         * loop above, so it can't recur. */
    }
#endif


    /* Read until \r\n\r\n appears in the unconsumed portion. The
     * scan starts from read_pos so already-buffered pipelined bytes
     * count toward the header boundary. */
    char* hdr_end = NULL;
    while (1) {
        if (conn->write_pos > conn->read_pos) {
            /* NUL-terminate the unconsumed slice in place — we have
             * one byte of slack reserved at the end of the buffer. */
            conn->buf[conn->write_pos] = '\0';
            hdr_end = strstr(conn->buf + conn->read_pos, "\r\n\r\n");
            if (hdr_end) break;
        }
        if (conn->write_pos + 1 >= conn->buf_cap) {
            /* Header section exceeded buffer capacity; bail. */
            return 0;
        }
        int n = conn_recv(conn, conn->buf + conn->write_pos,
                          conn->buf_cap - conn->write_pos - 1);
        if (n <= 0) {
            /* Nothing yet, and nothing of this request read so far: the
             * connection is idle rather than broken, and the caller can park
             * it instead of closing it (#1663). Anything mid-request stays a
             * close, because a half-read request cannot be resumed by a
             * different worker without carrying parse state with it. */
            if (n < 0 && conn->write_pos == conn->read_pos && conn_recv_timed_out())
                return HTTP_REQUEST_IDLE;
            return 0;  /* EOF / error */
        }
        conn->write_pos += n;
    }

    int header_size = (int)(hdr_end - conn->buf) + 4 - conn->read_pos;
    int request_total = header_size;

    /* Resolve Content-Length and ensure the full body is buffered. */
    const char* head_start = conn->buf + conn->read_pos;
    /* A request line longer than the parser's line buffer is answered rather
     * than dropped, and it is checked here so the parser's own bound is never
     * the thing a client reaches. */
    {
        const char* rl_end = (const char*)memchr(head_start, '\n',
                                                 (size_t)(hdr_end - head_start));
        size_t rl_len = rl_end ? (size_t)(rl_end - head_start)
                               : (size_t)(hdr_end - head_start);
        if (rl_len >= HTTP_MAX_REQUEST_LINE) {
            conn_send_uri_too_long(conn);
            return 0;
        }
    }
    /* Headers first: a line this server would read differently from the peer
     * that sent it is refused before its framing is trusted. */
    {
        const char* first_header = (const char*)memchr(head_start, '\n',
                                                       (size_t)(hdr_end - head_start));
        if (first_header) {
            int hdr_ok = conn_headers_well_formed(first_header + 1, hdr_end);
            if (hdr_ok < 0) { conn_send_headers_too_large(conn); return 0; }
            if (!hdr_ok)    { conn_send_bad_framing(conn);       return 0; }
        }
    }
    char cl_value[64];
    int cl_differing = 0;
    int cl_count = conn_find_header(head_start, hdr_end, "Content-Length",
                                    cl_value, sizeof(cl_value), &cl_differing);
    long content_length = 0;
    if (cl_count > 0) {
        /* Two lengths that disagree leave no single answer about where this
         * message ends, and a value that is not a count of bytes leaves none
         * either. Both are unrecoverable rather than something to pick from
         * (RFC 9112 6.3): guessing is what lets a front end and this server
         * disagree about where one request stops and the next begins. */
        if (cl_differing || conn_parse_content_length(cl_value, &content_length) != 0) {
            conn_send_bad_framing(conn);
            return 0;
        }
    }

    /* Transfer-Encoding decides where the body ends, and when it is present
     * Content-Length does not (RFC 9112 6.3). Ignoring it read a chunked
     * upload as having no body at all: the payload was dropped, and the chunk
     * bytes stayed in the stream to be parsed as the beginning of the next
     * request. Against a front end that does honour the header, that
     * disagreement about where one request ends is request smuggling.
     *
     * A message carrying both is refused rather than resolved, because the
     * two lengths are exactly what a smuggling pair is built from and no
     * legitimate sender emits both. */
    char te_value[128];
    int te_count = conn_find_header(head_start, hdr_end, "Transfer-Encoding",
                                    te_value, sizeof(te_value), NULL);
    int te_chunked = 0;
    if (te_count > 0) {
        te_chunked = http_strcasestr(te_value, "chunked") != NULL;
        /* Both lengths present, or a coding this server cannot decode: refuse
         * the message rather than pick one of the two answers. */
        if (cl_count > 0 || !te_chunked) {
            conn_send_bad_framing(conn);
            return 0;
        }
    }
    /* #626 (upload half) — streaming-body decision. A body whose
     * declared Content-Length exceeds one connection buffer (16 KiB)
     * is NOT fully buffered: we parse the headers, then hand the
     * handler a streaming request whose `http_request_body_read` pulls
     * bounded windows off the socket. This keeps peak server RAM at
     * O(buf + chunk) per connection instead of O(Content-Length) — the
     * difference between N×M-bytes-live and N×window-live for N
     * concurrent M-byte uploads. Bodies that fit in the buffer take the
     * legacy fully-buffered path (no behavioural change, and nothing to
     * gain by streaming them). The threshold is the buffer cap so a
     * "small" body is exactly "fits without growing the buffer". */
    int stream_body = (content_length > HTTP_CONN_BUF_CAP);

    /* A chunked body declares its length as it goes, so read until the
     * terminal chunk is in the buffer and then decode in place. The decoded
     * payload is shorter than the framing that carried it, so it fits where
     * the chunks were, and the request is left looking exactly like one that
     * arrived with a Content-Length. */
    if (te_chunked) {
        size_t frame_len = 0;
        for (;;) {
            int have = conn->write_pos - conn->read_pos - header_size;
            if (have > 0) {
                frame_len = http_chunked_frame_len(
                    conn->buf + conn->read_pos + header_size, (size_t)have);
                if (frame_len > 0) break;
                /* A chunked body declares no length up front, so the only
                 * bound on it is the one this server imposes. Without one, a
                 * sender that never emits the terminal chunk grows this
                 * buffer for as long as it cares to keep writing. */
                if (have > HTTP_MAX_CHUNKED_BODY) {
                    conn_send_payload_too_large(conn);
                    return 0;
                }
            }
            if (conn_buf_ensure(conn, conn->write_pos + HTTP_CONN_BUF_CAP + 1) != 0)
                return 0;
            int space = conn->buf_cap - conn->write_pos - 1;
            if (space <= 0) return 0;
            int n = conn_recv(conn, conn->buf + conn->write_pos, space);
            if (n <= 0) return 0;              /* closed or timed out mid-body */
            conn->write_pos += n;
            conn->buf[conn->write_pos] = '\0';
        }

        size_t decoded_len = 0;
        char* body_start = conn->buf + conn->read_pos + header_size;
        char* decoded = http_dechunk(body_start, frame_len, &decoded_len);
        if (!decoded) {
            conn_send_bad_framing(conn);
            return 0;
        }
        /* Anything past the terminal chunk is the next pipelined request, not
         * this body, and it has to survive the rewrite. The decoded payload is
         * never longer than the framing that carried it, so the tail moves
         * down and nothing overlaps what has not been copied out yet. */
        size_t tail_len = (size_t)(conn->write_pos - conn->read_pos - header_size)
                        - frame_len;
        if (tail_len > 0)
            memmove(body_start + decoded_len, body_start + frame_len, tail_len);
        memcpy(body_start, decoded, decoded_len);
        free(decoded);

        conn->write_pos = conn->read_pos + header_size
                        + (int)decoded_len + (int)tail_len;
        conn->buf[conn->write_pos] = '\0';
        content_length = (long)decoded_len;
        request_total  = header_size + (int)decoded_len;
        stream_body    = 0;
    }

    if (!te_chunked && content_length > 0 && !stream_body) {
        int needed_total = header_size + (int)content_length;
        /* Make sure the buffer can hold (read_pos + needed_total)
         * bytes plus a NUL. */
        if (conn_buf_ensure(conn, conn->read_pos + needed_total + 1) != 0) {
            return 0;
        }
        while (conn->write_pos - conn->read_pos < needed_total) {
            int want = needed_total - (conn->write_pos - conn->read_pos);
            int avail = conn->buf_cap - conn->write_pos - 1;
            int chunk = want < avail ? want : avail;
            if (chunk <= 0) return 0;
            int n = conn_recv(conn, conn->buf + conn->write_pos, chunk);
            if (n <= 0) return 0;
            conn->write_pos += n;
        }
        request_total = needed_total;
    }
    /* When streaming, `request_total` stays at `header_size` — we parse
     * only the header block now; the body bytes (the slice already in
     * the buffer plus everything still on the socket) are left for the
     * body-read accessor and the post-handler drain to consume. */

    /* Carve out the request slice. The length-aware `_n` entry point
     * gets the exact byte count so its body-parse clamps `Content-
     * Length` against what's actually buffered (defends against a
     * lying or buggy client header). We still NUL-terminate the slice
     * before the call because the request-line / header scan inside
     * `_n` uses strstr — restore the saved byte afterward so the next
     * request's bytes (already buffered from a pipelined recv)
     * survive. */
    char* req_start = conn->buf + conn->read_pos;
    char saved = req_start[request_total];
    req_start[request_total] = '\0';
    HttpRequest* req = http_parse_request_n(req_start, (size_t)request_total);
    req_start[request_total] = saved;

    /* Advance past the parsed bytes regardless of parse outcome —
     * a malformed request shouldn't make the same bytes parse again
     * on the next iteration. When streaming we advance only past the
     * headers; `read_pos` then points at the first body byte, which the
     * accessor reads from and the drain skips past. */
    conn->read_pos += request_total;
    if (!req) return 0;

    /* Wire up the streaming-body state so http_request_body_read can
     * pull the rest off the socket. body_length carries the declared
     * total so length-aware callers (http.request_body_length) still
     * report the wire size; `body` is NULL (no buffered payload). */
    if (stream_body) {
        /* The parser only saw the header slice, so it left a 0-byte
         * placeholder in req->body (malloc'd empty buffer). Drop it:
         * a streaming request's contract is body == NULL until either
         * http_request_body materializes it or the chunked accessor
         * consumes the wire directly. Leaving the placeholder in place
         * would make the whole-body accessor return an empty buffer
         * while body_length claims the declared Content-Length — a
         * 1-byte allocation paired with a multi-MB length. */
        free(req->body);
        req->body            = NULL;
        req->stream_conn     = conn;
        req->stream_total    = content_length;
        req->stream_consumed = 0;
        req->body_length     = (size_t)content_length;
    }


    /* Connection-level metadata that handlers learn from the kernel rather
     * than from the request bytes.
     *
     * Resolved ONCE per connection (#1719) — neither address can change
     * while the socket is open, and fetching them per request cost 2
     * syscalls, 2 inet_ntop calls and 2 strdups each time. nginx makes none
     * of those calls at all.
     *
     * - remote_addr/remote_port (getpeername): the trusted peer. The
     *   X-Forwarded-For header is client-supplied and a wrong basis for an
     *   allow/deny decision on a direct listener; this is the kernel's view
     *   of the socket, which is unspoofable.
     * - local_addr/local_port (getsockname): which NIC this accepted fd is
     *   bound to. Needed when the listener binds 0.0.0.0 and the handler
     *   gates on the receiving interface (admin-on-loopback, multi-tenant
     *   per-IP routing).
     * - is_tls: the connection wrapper, not anything in the wire bytes.
     *   Drives scheme/redirect/cookie-Secure decisions.
     *
     * The request keeps owning its copies — http_request_free frees both —
     * so the strdups stay and only the syscalls go. A failed lookup leaves
     * the cache empty and the accessors return ""/0, as before.
     *
     * IPv4 + IPv6 via sockaddr_storage. */
    {
        conn_resolve_addrs(conn);
        if (conn->remote_addr[0]) {
            req->remote_addr = strdup(conn->remote_addr);
            req->remote_port = conn->remote_port;
        }
        if (conn->local_addr[0]) {
            req->local_addr = strdup(conn->local_addr);
            req->local_port = conn->local_port;
        }
        req->is_tls = (conn->ssl != NULL) ? 1 : 0;
    }

    // Create response
    HttpServerResponse* res = http_response_create();
    res->takeover_conn = conn;

    // Execute middleware chain
    HttpMiddlewareNode* middleware = server->middleware_chain;
    int should_continue = 1;

    while (middleware && should_continue) {
        should_continue = middleware->middleware(req, res, middleware->user_data);
        middleware = middleware->next;
    }

    // If middleware blocked, send response and close.
    if (!should_continue) {
        if (res->takeover_taken) {
            http_request_free(req);
            http_server_response_free(res);
            return 0;
        }
        /* Same finish as the route-handler path: a middleware that answers
         * (static_files staging a sendfile FD, the reverse proxy returning an
         * upstream response) produces an ordinary response, and the
         * connection's fate is decided the same way. */
        return finish_response(server, conn, req, res, requests_served, max_requests);
    }

    /* h2c (cleartext HTTP/2) upgrade dispatch (#260 Tier 2 — RFC 7540 §3.2).
     * If the client offered `Upgrade: h2c` AND included a valid
     * HTTP2-Settings header AND we have h2 enabled, send 101
     * Switching Protocols, drop into the h2 driver pre-loaded with
     * stream 1 (this very request), and return 0 so the outer
     * keep-alive loop closes the HTTP/1.1 path. The h2 driver owns
     * the connection from here. */
#ifdef AETHER_HAS_NGHTTP2
    if (server->h2_enabled && !conn->is_h2) {
        const char* up = http_get_header(req, "Upgrade");
        const char* h2settings = http_get_header(req, "HTTP2-Settings");
        /* HTTP2-Settings can be empty (zero settings entries) per
         * RFC 7540 §3.2.1; we just need the header to be present.
         * NULL means the client didn't send it at all → not a
         * valid h2c upgrade → fall through to HTTP/1.1. */
        if (up && strcasecmp(up, "h2c") == 0 && h2settings) {
            /* 101 reply must precede any h2 frames on the wire. */
            const char* reply =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: h2c\r\n"
                "\r\n";
            if (conn_send(conn, reply, (int)strlen(reply)) <= 0) {
                http_request_free(req);
                http_server_response_free(res);
                return 0;
            }
            /* Reconstruct the original request's header block in
             * `Name: value\r\n` form so the wrapper synthesises a
             * stream 1 that mirrors the HTTP/1.1 request. */
            size_t hbuf_cap = 1024;
            char* hbuf = malloc(hbuf_cap);
            size_t hbuf_len = 0;
            if (hbuf) {
                for (int i = 0; i < req->header_count; i++) {
                    const char* k = req->header_keys[i];
                    const char* v = req->header_values[i];
                    if (!k || !v) continue;
                    /* Skip the upgrade-only headers — they don't
                     * belong on an h2 stream. */
                    if (strcasecmp(k, "Connection") == 0 ||
                        strcasecmp(k, "Upgrade") == 0 ||
                        strcasecmp(k, "HTTP2-Settings") == 0) continue;
                    size_t klen = strlen(k);
                    size_t vlen = strlen(v);
                    size_t need = klen + vlen + 4;
                    if (hbuf_len + need + 1 > hbuf_cap) {
                        size_t nc = hbuf_cap;
                        while (nc < hbuf_len + need + 1) nc *= 2;
                        char* nb = realloc(hbuf, nc);
                        if (!nb) break;
                        hbuf = nb; hbuf_cap = nc;
                    }
                    memcpy(hbuf + hbuf_len, k, klen);  hbuf_len += klen;
                    memcpy(hbuf + hbuf_len, ": ", 2);  hbuf_len += 2;
                    memcpy(hbuf + hbuf_len, v, vlen);  hbuf_len += vlen;
                    memcpy(hbuf + hbuf_len, "\r\n", 2); hbuf_len += 2;
                }
                if (hbuf) hbuf[hbuf_len] = '\0';
            }

            AetherH2Session* preloaded = aether_h2_session_from_h2c_upgrade(
                server, conn,
                req->method ? req->method : "GET",
                req->path   ? req->path   : "/",
                hbuf ? hbuf : "",
                h2settings);
            free(hbuf);

            http_request_free(req);
            http_server_response_free(res);

            if (preloaded) {
                conn->is_h2 = 1;
                handle_h2_connection(server, conn, preloaded);
            }
            return 0;
        }
    }
#endif

    /* WebSocket dispatch (#260 Tier 2 / E2). Match before SSE +
     * normal routes; require the Upgrade: websocket header to
     * confirm the client actually wants to upgrade (otherwise the
     * same path could serve a regular GET). */
    {
        const char* upgrade_hdr = http_get_header(req, "Upgrade");
        if (upgrade_hdr && strcasecmp(upgrade_hdr, "websocket") == 0) {
            struct HttpWsRoute* wr = server->ws_routes;
            while (wr) {
                if (req->path && wr->path && strcmp(wr->path, req->path) == 0) {
                    const char* key = http_get_header(req, "Sec-WebSocket-Key");
                    if (!key || !*key) {
                        http_response_set_status(res, 400);
                        http_response_set_body(res, "Sec-WebSocket-Key header missing");
                        size_t resp_len = 0;
                        char* response_str = http_response_serialize_len(res, &resp_len);
                        if (response_str) {
                            conn_send(conn, response_str, (int)resp_len);
                            free(response_str);
                        }
                        http_request_free(req);
                        http_server_response_free(res);
                        return 0;
                    }
                    if (ws_send_handshake(conn, key) != 0) {
                        http_request_free(req);
                        http_server_response_free(res);
                        return 0;  /* close */
                    }
                    HttpWsConn ws_handle = {
                        .conn = conn, .closed = 0,
                        .msg_buf = NULL, .msg_cap = 0,
                        .msg_len = 0, .msg_opcode = 0,
                    };
                    wr->handler(req, &ws_handle, wr->user_data);
                    /* If the handler returned without closing,
                     * send a normal-closure frame. */
                    if (!ws_handle.closed) {
                        http_ws_close(&ws_handle, 1000, "");
                    }
                    /* Cap-aware (#343): msg_buf was grown via
                     * ws_msg_grow (caps_realloc); msg_cap is the exact
                     * tracked size. NULL when no message arrived. */
                    aether_caps_free(ws_handle.msg_buf, (size_t)ws_handle.msg_cap);
                    http_request_free(req);
                    http_server_response_free(res);
                    return 0;
                }
                wr = wr->next;
            }
        }
    }

    /* SSE dispatch (#260 Tier 2). SSE routes own the connection
     * lifetime — the handler emits events directly to the wire and
     * the response struct is not used. Match before normal routes
     * so an /events SSE route takes precedence. */
    {
        struct HttpSseRoute* sr = server->sse_routes;
        while (sr) {
            if (req->path && sr->path && strcmp(sr->path, req->path) == 0) {
                /* Emit the SSE response head directly to the wire
                 * — bypassing http_response_set_body / serialize
                 * because we don't have a Content-Length and the
                 * body is open-ended. */
                const char* head =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                conn_send(conn, head, (int)strlen(head));

                HttpSseConn sse_handle = { .conn = conn, .closed = 0 };
                sr->handler(req, &sse_handle, sr->user_data);

                /* After the handler returns, flush whatever's
                 * pending in the response struct (it's empty —
                 * we never wrote to it) and tear down. SSE always
                 * forces close. */
                http_request_free(req);
                http_server_response_free(res);
                return 0;
            }
            sr = sr->next;
        }
    }

    HttpRoute* matched_route = http_resolve_route(server, req);

    // Execute route handler or return 404
    if (matched_route) {
        matched_route->handler(req, res, matched_route->user_data);
    } else {
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 Not Found");
    }

    if (res->takeover_taken) {
        http_request_free(req);
        http_server_response_free(res);
        return 0;
    }

    /* Run response-transformer chain (#260 Tier 1). Each transformer
     * may mutate the response — typical uses include gzip
     * compression and error-page substitution. They run in
     * registration order. */
    {
        struct HttpResponseTransformerNode* xform = server->response_transformer_chain;
        while (xform) {
            xform->xform(req, res, xform->user_data);
            xform = xform->next;
        }
    }

    /* Per-request observation hooks (#260 Tier 3). Fire after
     * transformers (so hooks see the final response state, e.g.
     * the gzip Content-Encoding) but before the wire send so the
     * latency measurement includes everything except network
     * write — the hooks are about server-side cost, not client-
     * perceived round trip. Hooks may not mutate; they observe. */
    if (server->request_hook_chain) {
        long t_end = http_now_us();
        long duration_us = t_end - t_start;
        struct HttpRequestHookNode* h = server->request_hook_chain;
        while (h) {
            h->hook(req, res, duration_us, h->user_data);
            h = h->next;
        }
    }

    /* #626 — drain any unread streaming-body bytes. A handler that
     * streamed only part of the body (or ignored it entirely — e.g. a
     * 413/401 rejection before reading) leaves the rest sitting on the
     * socket. On a keep-alive connection the next request parse would
     * then start mid-body and desync the wire. Consume whatever's left
     * of `stream_total` so `read_pos`/the socket sit exactly at the
     * next request boundary. Bounded scratch (no full-body buffer);
     * we read and discard. If the peer closed mid-body the drain just
     * stops — the keep-alive check below will see the broken stream on
     * the next recv and close. */
    if (req->stream_conn) {
        HttpConn* sconn = (HttpConn*)req->stream_conn;
        long left = req->stream_total - req->stream_consumed;
        /* First, discard any already-buffered body bytes. */
        if (left > 0) {
            int buffered = sconn->write_pos - sconn->read_pos;
            if (buffered > 0) {
                int drop = (buffered < (int)left) ? buffered : (int)left;
                sconn->read_pos += drop;
                req->stream_consumed += drop;
                left -= drop;
            }
        }
        /* Then read-and-discard the rest off the socket in buffer-sized
         * gulps (reusing the connection buffer's tail as scratch). */
        while (left > 0) {
            char scratch[4096];
            int chunk = (left < (long)sizeof(scratch)) ? (int)left : (int)sizeof(scratch);
            int n = conn_recv(sconn, scratch, chunk);
            if (n <= 0) break;
            req->stream_consumed += n;
            left -= n;
        }
        /* The streaming body is fully consumed (or the stream broke);
         * clear the backref so nothing touches it post-free. */
        req->stream_conn = NULL;
    }

    return finish_response(server, conn, req, res, requests_served, max_requests);
}

/* Public reusable helper. Owns the full per-connection lifecycle:
 * optional TLS handshake, request-parsing loop with keep-alive,
 * route dispatch, response emission, socket close. Used by both the
 * thread-pool worker path inside this file AND user actor step
 * functions registered via http_server_set_actor_handler — both
 * call here to get identical TLS / keep-alive / route-dispatch
 * behaviour. (#260 Tier 0 / Phase C3.)
 *
 * The inflight_connections counter (#260 Tier 3 graceful shutdown)
 * tracks active drains so http_server_shutdown_graceful can wait
 * for them to complete naturally before forcing close. */
/* Dispatch one fully-built request through the standard pipeline:
 * middleware → route lookup → handler → response transformers →
 * request hooks. Mutates `res` in place. Used both by the HTTP/1.1
 * loop in handle_one_request (which inlines this) and by the
 * HTTP/2 wrapper, which calls this function via extern from
 * std/http/server/h2/aether_h2.c so both protocols exercise the
 * same chain. (#260 Tier 2.) */
void http_server_dispatch_for_h2(HttpServer* server,
                                 HttpRequest* req,
                                 HttpServerResponse* res) {
    if (!server || !req || !res) return;
    long t_start = http_now_us();

    /* Middleware chain — same shape as the HTTP/1.1 path. */
    HttpMiddlewareNode* middleware = server->middleware_chain;
    int should_continue = 1;
    while (middleware && should_continue) {
        should_continue = middleware->middleware(req, res, middleware->user_data);
        middleware = middleware->next;
    }
    if (!should_continue) {
        return;  /* middleware blocked; res already populated */
    }

    HttpRoute* matched_route = http_resolve_route(server, req);
    if (matched_route) {
        matched_route->handler(req, res, matched_route->user_data);
    } else {
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 Not Found");
    }

    /* Response transformer chain (gzip / error_pages / etc.). */
    {
        struct HttpResponseTransformerNode* xform = server->response_transformer_chain;
        while (xform) {
            xform->xform(req, res, xform->user_data);
            xform = xform->next;
        }
    }

    /* Per-request observation hooks (#260 Tier 3). */
    if (server->request_hook_chain) {
        long duration_us = http_now_us() - t_start;
        struct HttpRequestHookNode* h = server->request_hook_chain;
        while (h) {
            h->hook(req, res, duration_us, h->user_data);
            h = h->next;
        }
    }
}

#ifdef AETHER_HAS_NGHTTP2
#include "../http/server/h2/aether_h2.h"

/* Wire-write callback for the h2 wrapper. The opaque user_data is
 * the HttpConn so we route through the same TLS-or-plain conn_send
 * path the rest of the server uses. */
static int h2_wire_write_cb(void* userdata,
                            const uint8_t* buf, size_t len) {
    HttpConn* conn = (HttpConn*)userdata;
    int n = conn_send(conn, (const char*)buf, (int)len);
    return (n < 0) ? -1 : n;
}

/* Handle an h2 connection from start to finish. The connection has
 * already been TLS-handshaken, h2c-upgraded, OR shown the h2
 * connection preface ("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", RFC 7540
 * §3.5) on a plain socket — caller is responsible for choosing the
 * right entry path. Returns when the session reports want_close or
 * the wire goes dead. */
static void handle_h2_connection(HttpServer* server, HttpConn* conn,
                                 AetherH2Session* preloaded) {
    AetherH2Session* sess = preloaded
        ? preloaded
        : aether_h2_session_new(server, conn);
    if (!sess) return;
    if (!preloaded) {
        aether_h2_session_send_initial_settings(sess);
    }

    /* If the HTTP/1.1 path already buffered bytes (e.g. the h2
     * connection preface that triggered us, or pipelined frames
     * after an h2c upgrade), feed them to nghttp2 first before
     * reading from the wire. */
    if (conn->buf && conn->write_pos > conn->read_pos) {
        size_t buffered = (size_t)(conn->write_pos - conn->read_pos);
        if (aether_h2_session_feed(sess,
                                   (const uint8_t*)(conn->buf + conn->read_pos),
                                   buffered) < 0) {
            aether_h2_session_free(sess);
            return;
        }
        conn->read_pos = conn->write_pos;  /* consumed */
    }

    /* Drain anything queued before reading (initial SETTINGS frame,
     * pre-loaded h2c stream-1 response). */
    if (aether_h2_session_drain(sess, h2_wire_write_cb) < 0) {
        aether_h2_session_free(sess);
        return;
    }

    uint8_t inbuf[16 * 1024];
    int goaway_sent = 0;
    int wake_fd = aether_h2_session_wake_fd(sess);  /* -1 when no pool */
    while (1) {
        /* Graceful shutdown bridge (#260 Tier 3 + h2). When
         * http_server_stop / http_server_shutdown_graceful flips
         * is_running to 0, we send GOAWAY (RFC 7540 §6.8) once so
         * the peer knows not to start new streams; in-flight
         * streams keep running until they finish naturally. After
         * the GOAWAY drains and all streams close, want_close
         * flips and we exit. */
        if (!goaway_sent && !server->is_running) {
            aether_h2_session_initiate_goaway(sess);
            goaway_sent = 1;
        }

        /* Drain any worker-completed responses BEFORE the wire
         * drain so their frames go out in this same iteration. */
        aether_h2_session_drain_ready(sess);

        int rc = aether_h2_session_drain(sess, h2_wire_write_cb);
        if (rc < 0) break;
        if (rc == 1) break;  /* clean close */

        if (aether_h2_session_want_close(sess)) break;

#if !defined(_WIN32)
        /* When concurrent dispatch is on, poll the socket AND the
         * wake pipe so a worker finishing mid-recv doesn't have to
         * wait for the next byte from the peer to get its response
         * onto the wire. wake_fd == -1 when the pool is off — fall
         * through to the plain blocking recv path. */
        if (wake_fd >= 0) {
            struct pollfd pfds[2];
            pfds[0].fd = conn->fd;       pfds[0].events = POLLIN; pfds[0].revents = 0;
            pfds[1].fd = wake_fd;         pfds[1].events = POLLIN; pfds[1].revents = 0;
            /* 1s timeout so the graceful-shutdown / is_running flag
             * gets re-checked at least once per second even when the
             * connection is otherwise idle. */
            int pr = poll(pfds, 2, 1000);
            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (pr == 0) continue;  /* timeout — re-loop */
            if (pfds[1].revents & POLLIN) {
                /* drain_ready will run at the top of the next loop
                 * iteration (it also drains the pipe). */
                continue;
            }
            if (!(pfds[0].revents & POLLIN)) continue;
            /* fall through to recv */
        }
#endif

        int n = conn_recv(conn, inbuf, (int)sizeof(inbuf));
        if (n <= 0) break;  /* EOF / timeout / error */
        if (aether_h2_session_feed(sess, inbuf, (size_t)n) < 0) break;
    }

    /* Final drain — any worker tasks still queued must complete or
     * be cancelled cleanly before the session goes away. drain_ready
     * is a no-op when no pool. */
    aether_h2_session_drain_ready(sess);

    /* Final drain so any GOAWAY / pending frames make it to the
     * peer before we tear down the session. */
    aether_h2_session_drain(sess, h2_wire_write_cb);
    aether_h2_session_free(sess);
}

/* Detect the HTTP/2 connection preface on a plain (non-TLS) socket
 * — RFC 7540 §3.5 says clients using prior-knowledge h2 send the
 * 24-byte magic "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" before any
 * frames. We peek at the leading bytes already in the conn buffer;
 * if they match, the caller switches the connection to the h2
 * driver. Returns 1 on match, 0 on no match (could still be h2c
 * via Upgrade — see the HTTP/1.1 path), -1 on insufficient data. */
static int conn_buffered_is_h2_preface(HttpConn* conn) {
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const int  preface_len = 24;
    int avail = conn->write_pos - conn->read_pos;
    if (!conn->buf) return -1;
    if (avail < preface_len) {
        /* If the available prefix already mismatches, no point
         * waiting for more — bail to HTTP/1.1 immediately. */
        for (int i = 0; i < avail; i++) {
            if (conn->buf[conn->read_pos + i] != preface[i]) return 0;
        }
        return -1;
    }
    return memcmp(conn->buf + conn->read_pos, preface, (size_t)preface_len) == 0;
}
#endif  /* AETHER_HAS_NGHTTP2 */

/* Close a connection the parking lot still owns, and release it. The lot
 * holds connections by pointer and has no view of what is inside one. */
void http_conn_close_owned(HttpConn* conn) {
    if (!conn) return;
    conn_close(conn);
    free(conn);
}

/* How long a worker waits for a follow-up request before handing the
 * connection to the parking lot, and only while a spare worker exists to take
 * other work. A client already mid-burst lands its next request in
 * microseconds, so this is long enough to keep a warm connection on its
 * worker, and short enough that an idle one does not hold that worker. */
#define HTTP_PARK_GRACE_MS 2

static int64_t conn_now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return (int64_t)time(NULL) * 1000;
}

/* Per-recv socket timeout, applied whenever a worker is about to block on
 * this connection. Also the idle guard against a slow-loris client. */
static void conn_apply_recv_timeout(HttpServer* server, HttpConn* c) {
    int fd = c->fd;
    int idle_ms = server->keep_alive_idle_ms > 0
        ? server->keep_alive_idle_ms : 30000;
    if (c->applied_recv_timeout_ms == idle_ms) return;
    c->applied_recv_timeout_ms = idle_ms;
#ifdef _WIN32
    DWORD rcv_timeout = (DWORD)idle_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&rcv_timeout, sizeof(rcv_timeout));
#else
    struct timeval rcv_tv = {
        .tv_sec  = idle_ms / 1000,
        .tv_usec = (idle_ms % 1000) * 1000
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
#endif
}

/* Is the next request already here, or about to be?
 *
 * Bytes in the connection's own buffer (a pipelined request, or an over-read
 * past the last one) and plaintext OpenSSL is holding both count and cost
 * nothing to check. Beyond that the answer is worth a brief wait only while a
 * worker is free: a client mid-burst sends again in microseconds and handing
 * that connection to the poller would be pure cost, but with every worker
 * busy the same wait is time another connection spends queued.
 *
 * Measured at 3000 requests per cell on an 8-core box, 16 workers: waiting
 * unconditionally gives 8,450 rps at 200 clients, parking on the instant
 * gives 29,800 at 8 clients, and this gives 86,000 and 30,300. */
static int conn_next_request_imminent(HttpConn* conn) {
    if (conn->write_pos > conn->read_pos) return 1;
#ifdef AETHER_HAS_OPENSSL
    if (conn->ssl && SSL_pending((SSL*)conn->ssl) > 0) return 1;
#endif

#if !defined(_WIN32)
    /* Take the next request rather than asking whether one is there.
     *
     * A poll that answers yes is followed by a read of the same socket, so on
     * a connection in use the poll is a syscall that learns what the read
     * would have told us. Reading straight into the connection buffer
     * collapses the pair: handle_one_request finds the bytes already there
     * and does not read again.
     *
     * Profiling put two thirds of this balancer's time in syscall entry, and
     * one poll per response is among the few syscalls nginx does not make.
     *
     * MSG_DONTWAIT rather than switching the socket to non-blocking: the flag
     * is free, where fcntl would cost two more syscalls than the poll it
     * replaces. TLS keeps the poll — SSL_read against a socket the library
     * believes is blocking is a different contract. */
    if (!conn->ssl && conn_buf_ensure(conn, HTTP_CONN_BUF_CAP) == 0) {
        int space = conn->buf_cap - conn->write_pos - 1;
        if (space > 0) {
            ssize_t n = recv(conn->fd, conn->buf + conn->write_pos,
                             (size_t)space, MSG_DONTWAIT);
            if (n > 0) { conn->write_pos += (int)n; return 1; }
            /* 0 is the peer closing; any error that is not "nothing yet" is a
             * connection that will not serve another request. Both mean do
             * not park it, which is what returning 0 does. */
            if (n == 0) return 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return 0;
        }
    }
#endif

    int grace_ms = http_pool_has_spare_worker() ? HTTP_PARK_GRACE_MS : 0;
#if !defined(_WIN32)
    struct pollfd pfd = { .fd = conn->fd, .events = POLLIN, .revents = 0 };
    return poll(&pfd, 1, grace_ms) > 0;
#else
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(conn->fd, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = grace_ms * 1000 };
    return select(conn->fd + 1, &rfds, NULL, NULL, &tv) > 0;
#endif
}

/* Run requests on a connection the caller owns until it closes or parks.
 * Frees the connection unless the lot took it. */
static void conn_serve(HttpServer* server, HttpConn* conn) {
    atomic_fetch_add(&server->inflight_connections, 1);
    HttpParkLot* lot = (HttpParkLot*)server->park_lot;
    /* With a lot to park into, a worker waits only briefly for the next
     * request before handing the connection over; the lot then holds it for
     * the real idle timeout. Without one, the socket timeout IS the idle
     * timeout, which is what it has always been.
     *
     * The wait is a plain blocking recv either way: a poll before it would be
     * a syscall per request, and measured at 8 concurrent clients that cost
     * 9% against simply blocking. */
    conn_apply_recv_timeout(server, conn);

#ifdef AETHER_HAS_NGHTTP2
    if (conn->is_h2) {
        handle_h2_connection(server, conn, NULL);
        conn_close(conn);
        free(conn);
        atomic_fetch_sub(&server->inflight_connections, 1);
        return;
    }
#endif

    int max_requests = server->keep_alive_max;
    int64_t idle_since_ms = 0;
    for (;;) {
        int rc = handle_one_request(server, conn, conn->requests_served, max_requests);
        if (rc == 0) break;

        if (rc == HTTP_REQUEST_IDLE) {
            /* No request arrived within the handoff window. Park it and free
             * the worker; the lot watches it for the rest of the idle
             * timeout. When there is no lot, or it declines (at capacity,
             * shutting down), keep waiting on this worker until the real idle
             * timeout, which is the pre-parking behaviour. */
            if (!lot) break;
            int idle_ms = server->keep_alive_idle_ms > 0
                ? server->keep_alive_idle_ms : 30000;
            if (idle_since_ms == 0) idle_since_ms = conn_now_ms();
            int64_t waited = conn_now_ms() - idle_since_ms;
            if (waited >= idle_ms) break;   /* silent past the deadline: close */
            if (http_park_add(lot, conn, (int)(idle_ms - waited)) == 0) {
                atomic_fetch_sub(&server->inflight_connections, 1);
                return;
            }
            continue;   /* lot declined: wait again on this worker */
        }

        idle_since_ms = 0;
        conn->requests_served++;
        if (max_requests > 0 && conn->requests_served >= max_requests) break;

        /* How long this worker will wait for the next request before handing
         * the connection to the lot, decided per response from whether a
         * worker is actually free.
         *
         * With one to spare, waiting costs nobody anything and saves a
         * handoff, so wait the full window: the read below is then exactly
         * the blocking recv this server has always done, with no extra
         * syscall on the hot path. With every worker busy, that wait is time
         * another connection spends queued, so shrink it to almost nothing
         * and let the poller hold this one instead.
         *
         * Measured at 3000 requests per cell on an 8-core box: waiting
         * unconditionally gives 89,000 rps at 8 clients but 8,450 at 200;
         * parking on the instant gives 29,800 at 8 and 30,300 at 200. The
         * window is only re-applied when it changes, so a steady connection
         * pays one setsockopt, not one per request. */
        /* Park unless the next request is already here. The check waits a
         * couple of milliseconds when a worker is free, because a client
         * mid-burst is the case keep-alive exists for and a handoff there is
         * pure cost; it waits not at all when every worker is busy, because
         * then the wait is time another connection spends queued. */
        if (lot && !conn_next_request_imminent(conn)) {
            int idle_ms = server->keep_alive_idle_ms > 0
                ? server->keep_alive_idle_ms : 30000;
            if (http_park_add(lot, conn, idle_ms) == 0) {
                atomic_fetch_sub(&server->inflight_connections, 1);
                return;
            }
        }
    }

    conn_close(conn);
    free(conn);
    atomic_fetch_sub(&server->inflight_connections, 1);
}

/* The parking lot woke this connection: it has data, so put it back on a
 * worker. Runs on the poller thread, so it must not serve the request here. */
void http_server_resume_connection(HttpServer* server, HttpConn* conn) {
    if (!server || !conn) return;
    conn_serve(server, conn);
}

void http_server_drain_connection(HttpServer* server, int client_fd) {
    if (!server || client_fd < 0) return;

    /* Heap-allocated because a parked connection outlives the worker that
     * parked it: the read buffer (with any bytes already pulled past the last
     * request), the TLS session and the request count all have to survive the
     * handoff (#1663). */
    HttpConn* conn = (HttpConn*)calloc(1, sizeof(HttpConn));
    if (!conn) { close(client_fd); return; }
    conn->fd = client_fd;
    conn->ssl = NULL;
    conn->is_h2 = 0;
    conn->buf = NULL;
    conn->buf_cap = 0;
    conn->read_pos = 0;
    conn->write_pos = 0;
    conn->requests_served = 0;
    /* calloc would leave this 0, which is a legitimate timeout meaning "block
     * indefinitely"; the guard must see "nothing applied yet" instead. */
    conn->applied_recv_timeout_ms = -1;

#ifdef AETHER_HAS_OPENSSL
    if (server->tls_enabled && server->tls_ctx) {
        if (conn_tls_accept(conn, (SSL_CTX*)server->tls_ctx) != 0) {
            close(client_fd);
            free(conn);
            return;
        }
    }
#endif
    conn_serve(server, conn);
}

/* The parking lot woke a connection: put it back on a worker rather than
 * serving it on the poller thread, which must stay free for the other parked
 * connections. */
static void http_park_resume(HttpServer* server, HttpConn* conn) {
    http_pool_submit_conn((HttpConnectionPool*)server->conn_pool, conn);
}

static void handle_client_connection(HttpServer* server, int client_fd) {
    /* Thread-pool path — defers to the public drain helper so the
     * keep-alive / TLS / route-dispatch logic lives in one place. */
    http_server_drain_connection(server, client_fd);
}


// ---------------------------------------------------------------------------
// Accept thread context (one per core in multi-accept mode)
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
typedef struct {
    HttpServer* server;
    int listen_fd;          // This thread's SO_REUSEPORT listen socket
    AetherIoPoller* poller; // This thread's I/O poller
    int thread_index;       // Which core's workers to prefer
} AcceptThreadCtx;

// Create a SO_REUSEPORT listen socket bound to the same port
static int create_reuseport_socket(const char* host, int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host, &addr.sin_addr);
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }

    // Non-blocking for epoll
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

// Dispatch a data-ready fd to a worker actor
static inline void dispatch_to_worker(HttpServer* server, int fd) {
    void* worker = server->spawn_fn(-1, server->step_fn, 0);
    if (worker) {
        HttpConnectionMessage conn_msg;
        conn_msg.type = MSG_HTTP_CONNECTION;
        conn_msg.client_fd = fd;
        server->send_fn(worker, &conn_msg, sizeof(conn_msg));
    } else {
        const char* err = "HTTP/1.1 503 Service Unavailable\r\n"
                          "Content-Length: 19\r\n\r\nService Unavailable";
        send(fd, err, strlen(err), MSG_NOSIGNAL);
        close(fd);
    }
}

// Per-core accept + I/O poller loop with optimistic recv
// Strategy: try MSG_PEEK on accept() — if data is already there (common for
// short-lived HTTP), dispatch immediately without touching the poller (saves syscalls).
// Only register with the poller if the client hasn't sent data yet.
static void accept_poller_loop(HttpServer* server, int listen_fd, AetherIoPoller* poller) {
    AetherIoEvent events[256];

    while (server->is_running) {
        int n = aether_io_poller_poll(poller, events, 256, 100);

        for (int i = 0; i < n; i++) {
            int fd = events[i].fd;

            if (fd == listen_fd) {
                // Re-register listen fd (one-shot semantics auto-removed it)
                aether_io_poller_add(poller, listen_fd, NULL, AETHER_IO_READ);

                // Accept all pending connections
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                        (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd < 0) break;

                    // Optimistic path: check if data already arrived (very common
                    // for HTTP — request follows TCP handshake immediately).
                    // This skips syscalls per connection.
                    char peek;
                    int peeked = recv(client_fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (peeked > 0) {
                        // Data ready — dispatch directly, no poller needed
                        dispatch_to_worker(server, client_fd);
                        continue;
                    }

                    // Slow path: no data yet, register with poller
                    int cflags = fcntl(client_fd, F_GETFL, 0);
                    if (cflags >= 0) fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK);

                    if (aether_io_poller_add(poller, client_fd, NULL, AETHER_IO_READ) != 0) {
                        close(client_fd);
                    }
                }
            } else {
                // Data ready (from poller, one-shot already removed) — dispatch
                dispatch_to_worker(server, fd);
            }
        }
    }
}

static void* accept_thread_fn(void* arg) {
    AcceptThreadCtx* ctx = (AcceptThreadCtx*)arg;
    accept_poller_loop(ctx->server, ctx->listen_fd, ctx->poller);
    free(ctx);
    return NULL;
}
#endif

int http_server_start_raw(HttpServer* server) {
    server->is_running = 1;

#if !defined(_WIN32)
    int use_actor_mode = (server->spawn_fn && server->send_fn && server->step_fn);
    if (use_actor_mode && server->multi_accept) {
        // Multi-accept mode (opt-in): one accept thread per core with SO_REUSEPORT.
        // Best for very high connection rates where accept() is the bottleneck.
        int n_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (n_threads <= 0) n_threads = 4;
        if (n_threads > 16) n_threads = 16;

        server->accept_listen_fds = calloc(n_threads, sizeof(int));
        server->accept_pollers = calloc(n_threads, sizeof(AetherIoPoller));
        server->accept_threads = calloc(n_threads, sizeof(pthread_t));
        if (!server->accept_listen_fds || !server->accept_pollers || !server->accept_threads) {
            fprintf(stderr, "Failed to allocate accept thread state\n");
            return -1;
        }

        for (int i = 0; i < n_threads; i++) {
            server->accept_listen_fds[i] = -1;
        }

        for (int i = 0; i < n_threads; i++) {
            server->accept_listen_fds[i] = create_reuseport_socket(
                server->host, server->port, server->max_connections);
            if (server->accept_listen_fds[i] < 0) {
                fprintf(stderr, "Failed to create SO_REUSEPORT socket for thread %d\n", i);
                return -1;
            }

            if (aether_io_poller_init(&server->accept_pollers[i]) < 0) {
                fprintf(stderr, "I/O poller init failed for thread %d\n", i);
                return -1;
            }

            aether_io_poller_add(&server->accept_pollers[i],
                                 server->accept_listen_fds[i], NULL, AETHER_IO_READ);
        }

        server->accept_thread_count = n_threads;

        if (!server->background) {
            printf("Server running at http://%s:%d (%d accept threads, SO_REUSEPORT)\n",
                   server->host, server->port, n_threads);
            printf("Press Ctrl+C to stop\n\n");
            fflush(stdout);
        }

        for (int i = 1; i < n_threads; i++) {
            AcceptThreadCtx* ctx = malloc(sizeof(AcceptThreadCtx));
            if (!ctx) {
                // The join loop below assumes exactly n_threads accept threads;
                // continuing with a NULL ctx would write through NULL here and
                // leave a never-created thread to join. Fail loudly instead.
                fprintf(stderr, "aether: failed to allocate accept-thread context\n");
                abort();
            }
            ctx->server = server;
            ctx->listen_fd = server->accept_listen_fds[i];
            ctx->poller = &server->accept_pollers[i];
            ctx->thread_index = i;
            pthread_create(&server->accept_threads[i], NULL, accept_thread_fn, ctx);
        }

        accept_poller_loop(server, server->accept_listen_fds[0],
                           &server->accept_pollers[0]);

        for (int i = 1; i < n_threads; i++) {
            pthread_join(server->accept_threads[i], NULL);
        }

        for (int i = 0; i < n_threads; i++) {
            aether_io_poller_destroy(&server->accept_pollers[i]);
            if (server->accept_listen_fds[i] >= 0) close(server->accept_listen_fds[i]);
        }
        free(server->accept_listen_fds);
        free(server->accept_pollers);
        free(server->accept_threads);
        server->accept_listen_fds = NULL;
        server->accept_pollers = NULL;
        server->accept_threads = NULL;
        server->accept_thread_count = 0;

    } else if (use_actor_mode) {
        // Single-accept with I/O poller (default): one accept thread waits for data
        // before dispatching to worker actors. Best for most workloads.
        // Skip the bind if the caller already bound synchronously (e.g.
        // an embedding host that bound with port 0 and read the resolved
        // port back via http_server_port before starting the background
        // accept loop) — re-binding an open socket would fail.
        if (server->socket_fd < 0 &&
            http_server_bind_raw(server, server->host, server->port) < 0) {
            return -1;
        }

        if (aether_io_poller_init(&server->accept_poller) < 0) {
            fprintf(stderr, "I/O poller init failed\n");
            return -1;
        }

        aether_io_poller_add(&server->accept_poller, server->socket_fd, NULL, AETHER_IO_READ);

        int flags = fcntl(server->socket_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(server->socket_fd, F_SETFL, flags | O_NONBLOCK);

        if (!server->background) {
            printf("Server running at http://%s:%d\n", server->host, server->port);
            printf("Press Ctrl+C to stop\n\n");
            fflush(stdout);
        }

        accept_poller_loop(server, server->socket_fd, &server->accept_poller);

        aether_io_poller_destroy(&server->accept_poller);

    } else
#endif
    {
        // Skip the bind if already bound synchronously (see the
        // use_actor_mode branch above for the embedding rationale).
        if (server->socket_fd < 0 &&
            http_server_bind_raw(server, server->host, server->port) < 0) {
            return -1;
        }

        if (!server->background) {
            printf("Server running at http://%s:%d\n", server->host, server->port);
            printf("Press Ctrl+C to stop\n\n");
            fflush(stdout);
        }

        /* on_start lifecycle hook (#260 Tier 3). Fires once after
         * the listen socket is bound, before the accept loop runs.
         * Typical use: log "ready", flip a readiness probe to 200. */
        if (server->on_start) {
            server->on_start(server, server->on_start_user_data);
        }

#if AETHER_HAS_THREADS
        HttpConnectionPool* pool = http_pool_create(server);
        server->conn_pool = pool;
        /* The lot resubmits woken connections into the same pool. Capacity is
         * descriptors, not threads, which is the whole point: a few thousand
         * idle keep-alive clients cost a table slot each. */
        if (pool) {
            server->park_lot = http_park_create(server, http_park_resume, 4096);
        }
#endif

        // Fallback: poll + thread pool (non-Linux or no actor handler)
        while (server->is_running) {
#if !defined(_WIN32)
            struct pollfd pfd = { .fd = server->socket_fd, .events = POLLIN };
            int ready = poll(&pfd, 1, 1000);
            if (ready <= 0) continue;
#endif

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server->socket_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (!server->is_running) break;
                continue;
            }

#if AETHER_HAS_THREADS
            /* Pool creation can fail only when no worker thread could be
             * started; handling inline then is slower but still correct. */
            if (pool) {
                http_pool_submit(pool, client_fd);
            } else {
                handle_client_connection(server, client_fd);
            }
#else
            handle_client_connection(server, client_fd);
#endif
        }

#if AETHER_HAS_THREADS
        /* Stop parking before the pool: the lot resubmits into it, and a
         * connection woken into a destroyed pool would be closed twice. */
        http_park_destroy((HttpParkLot*)server->park_lot);
        server->park_lot = NULL;
        http_pool_destroy(pool);
        server->conn_pool = NULL;
#endif

        /* on_stop lifecycle hook fires after the accept loop exits
         * but BEFORE socket cleanup (so the hook still sees a live
         * server struct). Typical use: flush logs, snapshot
         * metrics, flip readiness probe to 503. */
        if (server->on_stop) {
            server->on_stop(server, server->on_stop_user_data);
        }
    }

    return 0;
}

static void* http_server_background_main(void* arg) {
    HttpServer* server = (HttpServer*)arg;
#if !defined(_WIN32)
    /* Embedded/background server: block async signals on this thread and
     * every thread it spawns (the accept thread + pool workers inherit
     * this mask). The host application — not the server's worker threads
     * — should receive process-directed signals (SIGINT/SIGTERM, and
     * notably SIGURG, which Go-style supervisors use for preemption). A
     * library server intercepting them is a footgun, especially when
     * left running in the background under a sandboxed harness
     * (std-http-server-background-sigurg-poisons-harness.md). The
     * synchronous fault signals (SEGV/FPE/BUS/ABRT/TRAP) are deliberately
     * NOT blocked — blocking a signal raised by the thread itself is
     * undefined, and the opt-in crash handler must still see them. */
    sigset_t block;
    sigfillset(&block);
    sigdelset(&block, SIGSEGV);
    sigdelset(&block, SIGFPE);
    sigdelset(&block, SIGBUS);
    sigdelset(&block, SIGABRT);
    sigdelset(&block, SIGTRAP);
    sigdelset(&block, SIGILL);
    pthread_sigmask(SIG_BLOCK, &block, NULL);
#endif
    http_server_start_raw(server);
    return NULL;
}

int http_server_start_background_raw(HttpServer* server) {
    if (!server) return -1;
    /* Mark embedded mode before the thread starts so http_server_start_raw
     * suppresses the interactive "Press Ctrl+C to stop" banner. */
    server->background = 1;
    pthread_t tid;
    if (pthread_create(&tid, NULL, http_server_background_main, server) != 0) {
        server->background = 0;
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

void http_server_stop(HttpServer* server) {
    if (!server) return;

    server->is_running = 0;

#if !defined(_WIN32)
    // Destroy pollers to unblock poll/epoll_wait/kevent in accept threads
    for (int i = 0; i < server->accept_thread_count; i++) {
        if (server->accept_pollers) {
            aether_io_poller_destroy(&server->accept_pollers[i]);
        }
        if (server->accept_listen_fds && server->accept_listen_fds[i] >= 0) {
            close(server->accept_listen_fds[i]);
            server->accept_listen_fds[i] = -1;
        }
    }

    aether_io_poller_destroy(&server->accept_poller);
#endif

    if (server->socket_fd >= 0) {
#ifdef _WIN32
        closesocket(server->socket_fd);
        WSACleanup();
#else
        close(server->socket_fd);
#endif
        server->socket_fd = -1;
    }
}

void http_server_free(HttpServer* server) {
    if (!server) return;

    http_server_stop(server);


    free(server->host);
    
    // Free routes
    HttpRoute* route = server->routes;
    while (route) {
        HttpRoute* next = route->next;
        free(route->method);
        free(route->path_pattern);
        free(route);
        route = next;
    }
    
    // Free middleware
    HttpMiddlewareNode* middleware = server->middleware_chain;
    while (middleware) {
        HttpMiddlewareNode* next = middleware->next;
        free(middleware);
        middleware = next;
    }

    // Free response transformers
    {
        struct HttpResponseTransformerNode* xform = server->response_transformer_chain;
        while (xform) {
            struct HttpResponseTransformerNode* next = xform->next;
            free(xform);
            xform = next;
        }
    }

    // Free per-request observation hooks (#260 Tier 3 F1/F2). Note:
    // we deliberately don't free the hook's user_data here — that's
    // owned by the registering subsystem (access logger / metrics
    // collector). Those leak across server free, which is fine for
    // process-lifetime servers; a follow-up could add a destructor
    // hook to clean them up too.
    {
        struct HttpRequestHookNode* h = server->request_hook_chain;
        while (h) {
            struct HttpRequestHookNode* next = h->next;
            free(h);
            h = next;
        }
    }

    // Free SSE routes (#260 Tier 2)
    {
        struct HttpSseRoute* r = server->sse_routes;
        while (r) {
            struct HttpSseRoute* next = r->next;
            free(r->path);
            free(r);
            r = next;
        }
    }

    // Free WebSocket routes (#260 Tier 2 / E2)
    {
        struct HttpWsRoute* r = server->ws_routes;
        while (r) {
            struct HttpWsRoute* next = r->next;
            free(r->path);
            free(r);
            r = next;
        }
    }

    /* Free TLS context if one was loaded via http_server_set_tls_raw. */
#ifdef AETHER_HAS_OPENSSL
    if (server->tls_ctx) {
        SSL_CTX_free((SSL_CTX*)server->tls_ctx);
        server->tls_ctx = NULL;
    }
#endif

    free(server);
}

// MIME type detection based on file extension
const char* http_mime_type(const char* path) {
    if (!path) return "application/octet-stream";

    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    ext++; // Skip the dot

    // Common web MIME types
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, "js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, "json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(ext, "xml") == 0) return "application/xml; charset=utf-8";
    if (strcasecmp(ext, "txt") == 0) return "text/plain; charset=utf-8";

    // Images
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "webp") == 0) return "image/webp";

    // Fonts
    if (strcasecmp(ext, "woff") == 0) return "font/woff";
    if (strcasecmp(ext, "woff2") == 0) return "font/woff2";
    if (strcasecmp(ext, "ttf") == 0) return "font/ttf";
    if (strcasecmp(ext, "otf") == 0) return "font/otf";

    // Other
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    if (strcasecmp(ext, "zip") == 0) return "application/zip";
    if (strcasecmp(ext, "wasm") == 0) return "application/wasm";

    return "application/octet-stream";
}

// Serve a single file.
//
// Issue #383 zero-copy: on POSIX (Linux, macOS, BSDs), open the
// file and fstat it, then stash the fd + size on the response.
// The connection writer in handle_one_request takes the sendfile(2)
// fast path when eligible (cleartext + HTTP/1.1 + no Range request)
// and falls back to reading the fd into the response body
// otherwise. Body remains NULL until the fallback runs — eligible
// requests pay zero allocation for the body.
//
// Open + fstat (rather than stat + open) closes the stat-vs-content
// race: Content-Length is computed from the same file descriptor
// that sendfile will read, so byte count and header agree even if
// the file is rewritten between calls. The FD is owned by the
// response from this point — http_server_response_free closes it
// if no other path took ownership first.
//
// Windows uses the buffered fopen+fread path. The Win32 send-file
// equivalent (TransmitFile) is a Winsock primitive that doesn't
// share API shape with sendfile; out of scope for v1. The `close()`
// macro Windows uses to redirect the symbol at closesocket() also
// makes mixing socket and file fds messy on this side; the buffered
// path stays clean of the redefine.
void http_serve_file(HttpServerResponse* res, const char* filepath) {
#ifndef _WIN32
    int fd = open(filepath, O_RDONLY
#ifdef O_CLOEXEC
        | O_CLOEXEC
#endif
    );
    if (fd < 0) {
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 - File Not Found");
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        http_response_set_status(res, 500);
        http_response_set_body(res, "500 - Server Error");
        return;
    }

    /* Refuse directories — open(2) succeeds but reading would be
     * meaningless. The static-serve dispatcher already rewrites
     * `/` to `/index.html`; guard the direct call path too. */
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 - Not a regular file");
        return;
    }

    char content_length_buf[32];
    snprintf(content_length_buf, sizeof(content_length_buf),
             "%lld", (long long)st.st_size);

    http_response_set_status(res, 200);
    http_response_set_header(res, "Content-Type", http_mime_type(filepath));
    http_response_set_header(res, "Content-Length", content_length_buf);
    http_response_set_header(res, "Access-Control-Allow-Origin", "*");

    /* Stash the fd + size. handle_one_request decides at send time
     * whether the fast path is taken; either way the response now
     * owns the fd. */
    if (res->sendfile_fd >= 0) {
        /* Idempotent: replace any prior staged fd (rare — handler
         * called http_serve_file twice on the same response). */
        close(res->sendfile_fd);
    }
    /* Clear any prior body so the headers-only serialization on
     * the fast path doesn't emit stale content alongside the
     * sendfile-emitted body. */
    if (res->body) {
        aether_caps_free(res->body, res->body_cap);
        res->body = NULL;
        res->body_length = 0;
        res->body_cap = 0;
    }
    res->sendfile_fd = fd;
    res->sendfile_size = (long long)st.st_size;
#else
    /* Windows: buffered path. */
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 - File Not Found");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* Cap-aware (#343): file size is OS-supplied, unbounded. */
    size_t content_cap = (size_t)size + 1;
    char* content = (char*)aether_caps_malloc(content_cap);
    if (!content) {
        fclose(f);
        http_response_set_status(res, 500);
        http_response_set_body(res, "500 - Server Error");
        return;
    }
    size_t bytes_read = fread(content, 1, (size_t)size, f);
    fclose(f);
    if (bytes_read == 0 && size > 0) {
        aether_caps_free(content, content_cap);
        http_response_set_status(res, 500);
        http_response_set_body(res, "500 - Server Error");
        return;
    }
    content[bytes_read] = '\0';
    http_response_set_status(res, 200);
    http_response_set_header(res, "Content-Type", http_mime_type(filepath));
    http_response_set_header(res, "Access-Control-Allow-Origin", "*");
    /* Use the length-aware setter so binary content (gzip, images,
     * urandom — anything with embedded NULs) round-trips intact.
     * The strlen-based set_body would truncate at the first \0 in
     * the body — which the cross-platform sendfile test surfaced
     * on Windows when serving a 1 MiB urandom file. */
    http_response_set_body_n(res, content, (int)bytes_read);
    aether_caps_free(content, content_cap);
#endif
}

/* #641 — Range / 206 support for static file serving.
 *
 * Parses a single Range header of the form:
 *   bytes=START-END    [START, END] inclusive
 *   bytes=START-       [START, file_size-1]
 *   bytes=-SUFFIX      [file_size-SUFFIX, file_size-1]
 *
 * Multi-range (bytes=0-1,5-6 / multipart/byteranges) is intentionally
 * out of scope for v1 — single range covers zsync, resumable
 * downloads, and media seeking; the issue accepted that scope.
 *
 * Returns:
 *   1  → range parsed; *out_start / *out_end populated
 *   0  → no Range header; serve the whole file (200)
 *  -1  → Range present but unparseable / unsatisfiable
 *
 * Caller emits 416 on -1, 206 on 1, 200 on 0.
 */
static int parse_range_header(const char* range_hdr, long long file_size,
                              long long* out_start, long long* out_end) {
    if (!range_hdr) return 0;
    /* Must start with "bytes=". */
    if (strncasecmp(range_hdr, "bytes=", 6) != 0) return -1;
    const char* p = range_hdr + 6;
    while (*p == ' ') p++;
    /* Reject multi-range — a comma means more than one slice. */
    if (strchr(p, ',') != NULL) return -1;

    long long start = -1, end = -1;
    /* "-SUFFIX" form. */
    if (*p == '-') {
        char* endp = NULL;
        long long suffix = strtoll(p + 1, &endp, 10);
        if (suffix <= 0 || endp == p + 1) return -1;
        if (suffix > file_size) suffix = file_size;
        start = file_size - suffix;
        end = file_size - 1;
    } else {
        char* endp = NULL;
        start = strtoll(p, &endp, 10);
        if (endp == p || *endp != '-') return -1;
        p = endp + 1;
        if (*p == '\0' || *p == ' ' || *p == '\r' || *p == '\n') {
            /* "START-" form. */
            end = file_size - 1;
        } else {
            end = strtoll(p, &endp, 10);
            if (endp == p) return -1;
        }
    }
    if (start < 0 || end < start || start >= file_size) return -1;
    if (end >= file_size) end = file_size - 1;
    *out_start = start;
    *out_end = end;
    return 1;
}

/* Buffered Range-aware response. Reads only the requested slice and
 * emits 206 + Content-Range + Content-Length. Used by serve_static
 * when Range is present. The sendfile fast-path stays disabled for
 * Range requests (sendfile_eligible already rejects them); this
 * function does NOT stash an fd, just reads the slice into the
 * response body the way the Windows buffered path does. */
static void http_serve_file_range(HttpServerResponse* res, const char* filepath,
                                  long long start, long long end,
                                  long long file_size) {
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 - File Not Found");
        return;
    }
    if (fseek(f, (long)start, SEEK_SET) != 0) {
        fclose(f);
        http_response_set_status(res, 500);
        http_response_set_body(res, "500 - Server Error");
        return;
    }
    long long slice_len = end - start + 1;
    size_t alloc = (size_t)slice_len + 1;
    char* buf = (char*)aether_caps_malloc(alloc);
    if (!buf) {
        fclose(f);
        http_response_set_status(res, 500);
        http_response_set_body(res, "500 - Server Error");
        return;
    }
    size_t got = fread(buf, 1, (size_t)slice_len, f);
    fclose(f);
    if (got != (size_t)slice_len) {
        aether_caps_free(buf, alloc);
        http_response_set_status(res, 500);
        http_response_set_body(res, "500 - Server Error");
        return;
    }
    buf[got] = '\0';

    char cr_buf[96];
    snprintf(cr_buf, sizeof(cr_buf), "bytes %lld-%lld/%lld",
             start, end, file_size);
    char cl_buf[32];
    snprintf(cl_buf, sizeof(cl_buf), "%lld", slice_len);

    http_response_set_status(res, 206);
    http_response_set_header(res, "Content-Type", http_mime_type(filepath));
    http_response_set_header(res, "Content-Range", cr_buf);
    http_response_set_header(res, "Content-Length", cl_buf);
    http_response_set_header(res, "Accept-Ranges", "bytes");
    http_response_set_header(res, "Access-Control-Allow-Origin", "*");
    http_response_set_body_n(res, buf, (int)slice_len);
    aether_caps_free(buf, alloc);
}

// Static file serving handler (for use with wildcard routes)
void http_serve_static(HttpRequest* req, HttpServerResponse* res, void* base_dir) {
    const char* dir = (const char*)base_dir;
    if (!dir) dir = ".";

    // Build filepath from request path
    const char* req_path = req->path;
    if (!req_path || req_path[0] == '\0') req_path = "/";

    // Handle root path
    if (strcmp(req_path, "/") == 0) {
        req_path = "/index.html";
    }

    // Skip leading slash
    if (req_path[0] == '/') req_path++;

    // Security: reject encoded traversal sequences (%2e, %2f, %5c)
    if (strstr(req_path, "..") != NULL ||
        strstr(req_path, "%2e") != NULL || strstr(req_path, "%2E") != NULL ||
        strstr(req_path, "%2f") != NULL || strstr(req_path, "%2F") != NULL ||
        strstr(req_path, "%5c") != NULL || strstr(req_path, "%5C") != NULL ||
        strstr(req_path, "\\") != NULL) {
        http_response_set_status(res, 403);
        http_response_set_body(res, "403 - Forbidden");
        return;
    }

    // Build full path
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir, req_path);

    // Security: resolve to canonical path and verify it's within the root dir
#ifndef _WIN32
    char resolved[PATH_MAX];
    char resolved_dir[PATH_MAX];
    if (!realpath(filepath, resolved) || !realpath(dir, resolved_dir)) {
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 - Not Found");
        return;
    }
    if (strncmp(resolved, resolved_dir, strlen(resolved_dir)) != 0) {
        http_response_set_status(res, 403);
        http_response_set_body(res, "403 - Forbidden");
        return;
    }
    // #641: if the request carries a Range header, serve a slice
    // (206) instead of the whole file (200). Also always advertise
    // Accept-Ranges on a successful 200 file response so clients
    // know they can issue Range follow-ups.
    const char* range_hdr = http_get_header(req, "Range");
    if (range_hdr) {
        struct stat st;
        if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) {
            long long file_size = (long long)st.st_size;
            long long rs = 0, re = 0;
            int rv = parse_range_header(range_hdr, file_size, &rs, &re);
            if (rv > 0) {
                http_serve_file_range(res, resolved, rs, re, file_size);
                return;
            }
            if (rv < 0) {
                /* Unsatisfiable / malformed → 416 with
                 * Content-Range: bytes */ /*total per RFC 7233 §4.4. */
                char cr_buf[64];
                snprintf(cr_buf, sizeof(cr_buf), "bytes */%lld", file_size);
                http_response_set_status(res, 416);
                http_response_set_header(res, "Content-Range", cr_buf);
                http_response_set_header(res, "Accept-Ranges", "bytes");
                http_response_set_body(res, "416 - Range Not Satisfiable");
                return;
            }
        }
        /* stat failed; fall through to http_serve_file which will
         * return 404. */
    }
    // Serve the resolved, validated path (200 + Accept-Ranges).
    http_serve_file(res, resolved);
    http_response_set_header(res, "Accept-Ranges", "bytes");
#else
    // On Windows, use _fullpath for canonicalization
    char resolved[1024];
    char resolved_dir[1024];
    if (!_fullpath(resolved, filepath, sizeof(resolved)) ||
        !_fullpath(resolved_dir, dir, sizeof(resolved_dir))) {
        http_response_set_status(res, 404);
        http_response_set_body(res, "404 - Not Found");
        return;
    }
    if (_strnicmp(resolved, resolved_dir, strlen(resolved_dir)) != 0) {
        http_response_set_status(res, 403);
        http_response_set_body(res, "403 - Forbidden");
        return;
    }
    // #641: Range support on Windows too. Same shape as POSIX.
    const char* range_hdr_w = http_get_header(req, "Range");
    if (range_hdr_w) {
        struct stat st;
        if (stat(resolved, &st) == 0) {
            long long file_size = (long long)st.st_size;
            long long rs = 0, re = 0;
            int rv = parse_range_header(range_hdr_w, file_size, &rs, &re);
            if (rv > 0) {
                http_serve_file_range(res, resolved, rs, re, file_size);
                return;
            }
            if (rv < 0) {
                char cr_buf[64];
                snprintf(cr_buf, sizeof(cr_buf), "bytes */%lld", file_size);
                http_response_set_status(res, 416);
                http_response_set_header(res, "Content-Range", cr_buf);
                http_response_set_header(res, "Accept-Ranges", "bytes");
                http_response_set_body(res, "416 - Range Not Satisfiable");
                return;
            }
        }
    }
    http_serve_file(res, resolved);
    http_response_set_header(res, "Accept-Ranges", "bytes");
#endif
}

// ============================================================================
// Actor dispatch mode
// ============================================================================

void http_server_set_actor_handler(HttpServer* server, void (*step_fn)(void*),
                                    void (*send_fn)(void*, void*, size_t),
                                    void* (*spawn_fn)(int, void (*)(void*), size_t),
                                    void (*release_fn)(void*)) {
    if (!server || !step_fn || !send_fn || !spawn_fn) return;
    server->step_fn = step_fn;
    server->send_fn = send_fn;
    server->spawn_fn = spawn_fn;
    server->release_fn = release_fn;
}

// Request accessors (for Aether .ae code via opaque ptr)
// Request field accessors. Each guards against NULL at both the struct
// and the field level so a partially-populated HttpRequest (e.g. one
// built by a C dispatch shim that didn't touch every field) doesn't
// crash downstream string ops. Empty string is the "absent" sentinel
// — consistent with Aether's Go-style string conventions.
const char* http_request_method(HttpRequest* req) {
    return (req && req->method) ? req->method : "";
}

const char* http_request_path(HttpRequest* req) {
    return (req && req->path) ? req->path : "";
}

const char* http_request_body(HttpRequest* req) {
    if (!req) return "";
    /* #644 — v1 contract on a STREAMING request: `http.request_body` must
     * keep returning the whole payload even though the dispatcher no longer
     * pre-buffers it. Materialize on demand: drain the remaining wire bytes
     * into a heap buffer once, then serve it like a buffered body. The
     * caller explicitly asked for full buffering, so the O(Content-Length)
     * allocation is the requested behaviour — handlers that want bounded
     * RAM use http_request_body_read instead.
     *
     * Mixed usage (some bytes already pulled via http_request_body_read)
     * cannot be honoured — the consumed prefix is gone, and returning a
     * tail as if it were the whole body would silently corrupt. That case
     * keeps returning "" (same as before this fix). */
    if (req->stream_conn && !req->body && req->stream_consumed == 0 &&
        req->stream_total > 0) {
        HttpConn* conn = (HttpConn*)req->stream_conn;
        long total = req->stream_total;
        char* buf = (char*)malloc((size_t)total + 1);
        if (!buf) return "";
        long got = 0;
        /* (a) body bytes that arrived in the same recv as the headers. */
        int buffered = conn->write_pos - conn->read_pos;
        if (buffered > 0) {
            long take = ((long)buffered < total) ? (long)buffered : total;
            memcpy(buf, conn->buf + conn->read_pos, (size_t)take);
            conn->read_pos += (int)take;
            got += take;
        }
        /* (b) the rest straight off the socket. A short read (peer closed
         * mid-body) yields the received prefix; the post-handler drain
         * sees the updated stream_consumed and gives up on the same
         * peer-close. */
        while (got < total) {
            int n = conn_recv(conn, buf + got, (int)(total - got));
            if (n <= 0) break;
            got += n;
        }
        buf[got] = '\0';
        req->body = buf;
        req->body_length = (size_t)got;
        req->stream_consumed += got;
    }
    return (req->body) ? req->body : "";
}

int http_request_body_length(HttpRequest* req) {
    if (!req) return 0;
    /* A materialized (or originally buffered) body reports its actual
     * byte count — after a short read this is what request_body holds. */
    if (req->body) return (int)req->body_length;
    /* Streaming request not yet materialized: `body` is NULL but the
     * declared length is the Content-Length we stashed. The canonical
     * upload loop reads this to know how many bytes to pull via
     * http_request_body_read. */
    if (req->stream_conn) return (int)req->stream_total;
    return 0;
}

/* #644 — has the request body fully arrived (and, for a streaming
 * request, been consumed off the wire)? Buffered requests are complete
 * by construction — the dispatcher had the whole payload before the
 * handler ran. A streaming request reports 1 once every declared byte
 * has been pulled (via http_request_body_read or a materializing
 * http_request_body call). Lets a chunked-iteration loop detect "done"
 * without comparing offsets against request_body_length. */
int http_request_body_complete(HttpRequest* req) {
    if (!req) return 0;
    if (req->stream_conn) {
        return (req->stream_consumed >= req->stream_total) ? 1 : 0;
    }
    return 1;
}

/* #626 — chunked-read request body, TLS split-accessor shape.
 *
 * This is the API shape the fbs-core port asked for:
 *
 *   ok = http_request_body_read_raw(req, offset, max);
 *   bytes_ptr = http_get_request_body_read();
 *   n         = http_get_request_body_read_length();
 *   ...handle the chunk...
 *   http_release_request_body_read();
 *
 * The Aether wrapper bundles this into a (bytes, n, err) tuple that
 * destructures the same way fs.read_binary does.
 *
 * TWO BACKING MODES (#626 upload half, now implemented):
 *   - Small bodies (Content-Length <= 16 KiB) are fully buffered into
 *     req->body at parse time; this accessor returns a random-access
 *     slice of that buffer (offset may be any value < total).
 *   - Large bodies are NOT buffered. The dispatcher parses only the
 *     headers and marks the request streaming (req->stream_conn set);
 *     this accessor then pulls the next window straight off the socket.
 *     Reads must be SEQUENTIAL (offset == bytes already consumed) — the
 *     socket isn't seekable. Peak server RAM stays at O(buf + window)
 *     per connection instead of O(Content-Length), which is the win the
 *     fbs-core port measured (N concurrent M-byte uploads no longer hold
 *     N×M live). The canonical loop is offset-forward, which satisfies
 *     the sequential constraint naturally. */
static __thread unsigned char* g_rbr_buf = NULL;
static __thread size_t         g_rbr_cap = 0;
static __thread int            g_rbr_len = 0;

static void release_rbr_locked(void) {
    /* Cap-aware (#343): every g_rbr_buf allocation records its exact
     * byte count in g_rbr_cap, so the matching free recovers the size
     * here. The buffer is allocated and freed entirely within this
     * module's request-body-read accessor surface; an upload-reading
     * plugin drives `max`/`want`, so the caps allocator bounds it. */
    if (g_rbr_buf) aether_caps_free(g_rbr_buf, g_rbr_cap);
    g_rbr_buf = NULL;
    g_rbr_cap = 0;
    g_rbr_len = 0;
}

void http_release_request_body_read(void) {
    release_rbr_locked();
}

const char* http_get_request_body_read(void) {
    return g_rbr_buf ? (const char*)g_rbr_buf : "";
}

int http_get_request_body_read_length(void) {
    return g_rbr_len;
}

/* Yield an EOF result (1-byte buffer, length 0) into the TLS slot. */
static int rbr_yield_eof(void) {
    g_rbr_buf = (unsigned char*)aether_caps_malloc(1);
    if (!g_rbr_buf) return 0;
    g_rbr_cap = 1;
    g_rbr_buf[0] = 0;
    g_rbr_len = 0;
    return 1;
}

int http_request_body_read_raw(HttpRequest* req, int offset, int max) {
    release_rbr_locked();
    if (!req || offset < 0 || max < 0) return 0;

    /* #626 streaming path. When the dispatcher chose not to buffer the
     * whole body, pull the next window straight off the socket. The
     * read is SEQUENTIAL: `offset` must equal how much has already been
     * consumed (the canonical loop walks `off` forward by the returned
     * `n`). A non-sequential offset on a streaming body can't be
     * honoured — the socket isn't seekable — so it yields EOF rather
     * than silently returning wrong bytes. Bytes come first from
     * whatever already sits in the connection buffer (arrived in the
     * same recv as the headers), then from fresh `conn_recv` calls,
     * always bounded by `max`. */
    if (req->stream_conn) {
        HttpConn* conn = (HttpConn*)req->stream_conn;
        long remaining = req->stream_total - req->stream_consumed;
        if (remaining <= 0) return rbr_yield_eof();
        if ((long)offset != req->stream_consumed) {
            /* Out-of-order read against an unbuffered stream — refuse. */
            return rbr_yield_eof();
        }

        long want = (max < remaining) ? (long)max : remaining;
        if (want <= 0) return rbr_yield_eof();
        g_rbr_buf = (unsigned char*)aether_caps_malloc((size_t)want);
        if (!g_rbr_buf) return 0;
        g_rbr_cap = (size_t)want;

        long got = 0;
        /* (a) drain any body bytes already in the connection buffer. */
        int buffered = conn->write_pos - conn->read_pos;
        if (buffered > 0) {
            int take = (buffered < (int)(want - got)) ? buffered : (int)(want - got);
            memcpy(g_rbr_buf + got, conn->buf + conn->read_pos, (size_t)take);
            conn->read_pos += take;
            got += take;
        }
        /* (b) read the rest straight off the socket, bounded by want. */
        while (got < want) {
            int chunk = (int)(want - got);
            int n = conn_recv(conn, g_rbr_buf + got, chunk);
            if (n <= 0) break;   /* short read / peer closed mid-body */
            got += n;
        }
        if (got <= 0) { release_rbr_locked(); return rbr_yield_eof(); }
        req->stream_consumed += got;
        g_rbr_len = (int)got;
        return 1;
    }

    /* Legacy fully-buffered path: random-access slice of req->body. */
    int total = (req->body) ? (int)req->body_length : 0;
    if (offset >= total) {
        return rbr_yield_eof();
    }
    int avail = total - offset;
    int want = (max < avail) ? max : avail;
    size_t alloc = want > 0 ? (size_t)want : 1;
    g_rbr_buf = (unsigned char*)aether_caps_malloc(alloc);
    if (!g_rbr_buf) return 0;
    g_rbr_cap = alloc;
    if (want > 0) memcpy(g_rbr_buf, req->body + offset, (size_t)want);
    g_rbr_len = want;
    return 1;
}

const char* http_request_query(HttpRequest* req) {
    return (req && req->query_string) ? req->query_string : "";
}

const char* http_request_remote_addr(HttpRequest* req) {
    /* Trusted TCP peer address. Populated by the dispatcher after
     * `http_parse_request_n` via `getpeername(2)` + `inet_ntop`;
     * stays NULL when the connection has no IP peer (Unix-domain
     * socket) or when the kernel call failed (the empty string
     * keeps the FFI shape stable for Aether callers).
     *
     * Intentionally separate from the X-Forwarded-For header path:
     * the header is client-supplied (use the
     * `std.http.middleware.use_real_ip` middleware behind a trusted
     * proxy that overwrites it); this accessor is what the kernel
     * actually sees on the socket, which is the only basis for a
     * directly-exposed listener's source-IP allow/deny decision. */
    return (req && req->remote_addr) ? req->remote_addr : "";
}

int http_request_remote_port(HttpRequest* req) {
    return req ? req->remote_port : 0;
}

const char* http_request_local_addr(HttpRequest* req) {
    /* Companion of `http_request_remote_addr` populated from
     * `getsockname(2)`. The listener may have been bound to the
     * wildcard 0.0.0.0 (or `::`) — in that case the accepted fd
     * still carries a concrete local address for each connection,
     * which is the only way to learn which NIC fielded the
     * request. */
    return (req && req->local_addr) ? req->local_addr : "";
}

int http_request_local_port(HttpRequest* req) {
    return req ? req->local_port : 0;
}

const char* http_request_scheme(HttpRequest* req) {
    /* "https" when the bytes arrived over a TLS-wrapped connection,
     * "http" otherwise. Source of truth is `conn->ssl != NULL`
     * captured at parse time; cleaner than re-deriving from a
     * Forwarded / X-Forwarded-Proto header (those describe the
     * edge proxy's view, not what THIS server saw). When the
     * server sits behind a TLS-terminating proxy, the
     * client-facing scheme can still be recovered from the
     * X-Forwarded-Proto header; this accessor is for the local
     * truth used by canonical URL building, redirect targets,
     * and `Set-Cookie ... Secure` decisions. */
    return (req && req->is_tls) ? "https" : "http";
}

int http_request_is_tls(HttpRequest* req) {
    return (req && req->is_tls) ? 1 : 0;
}

const char* http_request_http_version(HttpRequest* req) {
    /* The version slot the request-line / pseudo-header parser
     * already captured ("HTTP/1.0" / "HTTP/1.1" / "HTTP/2.0").
     * Surfaces a field that was always there but unreachable
     * from Aether. "" when the parser hasn't populated it yet
     * (defensive — every request that reaches a handler has
     * passed parsing, so this is unlikely in practice). */
    return (req && req->http_version) ? req->http_version : "";
}

// Request-header iteration (vcr_request_header_iteration_wish.md). Unlike
// http_get_header's named lookup, these enumerate every received header
// so a handler can capture an unknown set (e.g. a faithful VCR recorder).
// Order is the parser's insertion/wire order with duplicates preserved —
// no sort/dedup/canonicalization here; that's consumer policy. Indices
// out of [0, count) return "" (the same 50-header cap as parsing).
int http_request_header_count(HttpRequest* req) {
    return req ? req->header_count : 0;
}

const char* http_request_header_name(HttpRequest* req, int index) {
    if (!req || !req->header_keys || index < 0 || index >= req->header_count) return "";
    return req->header_keys[index] ? req->header_keys[index] : "";
}

const char* http_request_header_value(HttpRequest* req, int index) {
    if (!req || !req->header_values || index < 0 || index >= req->header_count) return "";
    return req->header_values[index] ? req->header_values[index] : "";
}

#endif // AETHER_HAS_NETWORKING
