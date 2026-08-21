#ifndef AETHER_HTTP_SERVER_H
#define AETHER_HTTP_SERVER_H

#include "../string/aether_string.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include <stdatomic.h>
#include <stddef.h>  // size_t
#include <time.h>    // struct tm (http_format_clf_time)

// HTTP Request
typedef struct {
    char* method;           // GET, POST, PUT, DELETE, etc.
    char* path;             // /api/users
    char* query_string;     // ?key=value&foo=bar
    char* http_version;     // HTTP/1.1
    char** header_keys;
    char** header_values;
    int header_count;
    char* body;
    size_t body_length;

    // Parsed data
    char** param_keys;      // From /users/:id
    char** param_values;
    int param_count;

    // Lazily parsed from query_string on first http_get_query_param() call.
    // query_parsed=0 means "not yet attempted"; ?0 is also the sentinel for
    // a NULL query_string (then count stays 0). Each value is its own
    // strdup so http_get_query_param can hand out a stable borrowed pointer
    // across the request's lifetime without a static-buffer collision.
    char** query_keys;
    char** query_values;
    int query_count;
    int query_parsed;
    /* Connection-level metadata populated by the dispatcher right after
     * `http_parse_request_n` from `getpeername(2)` / `getsockname(2)` /
     * `conn->ssl`. Tracked at the end of the struct so existing C
     * consumers — `tests/integration/http_external_ptr`'s shim,
     * downstream embedders that manufacture an HttpRequest in C — stay
     * binary-compatible if they fail to update. Same ABI promise the
     * query-cache fields above kept when they were added. Accessors
     * (`http_request_remote_addr`, `_remote_port`, `_local_addr`,
     * `_local_port`, `_scheme`, `_is_tls`) all tolerate the
     * NULL / zero defaults a C consumer leaves behind. */
    char* remote_addr;
    int   remote_port;
    char* local_addr;
    int   local_port;
    int   is_tls;
    /* Streaming request body (#626 upload half). When a request's
     * Content-Length exceeds the streaming threshold, the dispatcher
     * does NOT buffer the whole body — it parses headers + whatever
     * body bytes arrived in the same recv, then sets these fields so
     * `http_request_body_read` can pull the rest off the socket in
     * bounded windows. `stream_conn` is an opaque `HttpConn*` (kept
     * void* so the public header doesn't leak the connection struct);
     * NULL on a non-streaming request, where `body` holds the whole
     * payload as before. `stream_total` is the declared Content-Length;
     * `stream_consumed` advances as the accessor / post-handler drain
     * pulls bytes. Trailing fields → same ABI-stable promise as the
     * connection-metadata block above; a C consumer that leaves them
     * NULL/0 just gets the legacy fully-buffered behaviour. */
    void* stream_conn;
    long  stream_total;
    long  stream_consumed;
} HttpRequest;

// HTTP Response
typedef struct {
    int status_code;
    char* status_text;
    char** header_keys;
    char** header_values;
    int header_count;
    char* body;
    size_t body_length;
    /* Resource-caps tracker companion (#343): the byte count
     * aether_caps_free needs at the matching free site. Distinct
     * from body_length because every alloc is body_length+1 (NUL
     * terminator) and may be reused/reset across set_body calls;
     * tracking the cap explicitly keeps the cap counter at
     * current-usage even when callers replace the body. */
    size_t body_cap;
    /* Issue #383 zero-copy fast path. When the response body lives
     * on disk (set by http_serve_file), the file is `open(2)`'d
     * here and `fstat(2)`'d into sendfile_size. The connection
     * writer in handle_one_request decides at send time whether
     * the connection is eligible for sendfile(2) (cleartext +
     * HTTP/1.1 + no Range request) — eligible: serialize headers
     * only, then sendfile the body and close the fd. Ineligible:
     * read the fd into res->body and fall through to the buffered
     * serialize+send path. http_response_free closes the fd if
     * still open, so the FD is owned by the response struct from
     * the moment http_serve_file returns until response cleanup.
     * sendfile_fd == -1 means no zero-copy path is staged. */
    int       sendfile_fd;
    long long sendfile_size;
    /* CONNECT / protocol-upgrade takeover (#1086). The HTTP/1.1
     * dispatcher sets takeover_conn to its opaque HttpConn while the
     * route handler runs. http_response_accept_tunnel sends the
     * current response head, wraps the accepted fd as a std.tcp socket,
     * sets takeover_taken, and clears the dispatcher's fd so the
     * normal HTTP lifecycle will not serialize another response or
     * close a socket now owned by the handler. */
    void* takeover_conn;
    int   takeover_taken;
} HttpServerResponse;

// Route handler callback
typedef void (*HttpHandler)(HttpRequest* req, HttpServerResponse* res, void* user_data);

// Middleware callback
typedef int (*HttpMiddleware)(HttpRequest* req, HttpServerResponse* res, void* user_data);

// Route definition
typedef struct HttpRoute {
    char* method;
    char* path_pattern;     // /users/:id
    HttpHandler handler;
    void* user_data;
    struct HttpRoute* next;
} HttpRoute;

// Middleware chain
typedef struct HttpMiddlewareNode {
    HttpMiddleware middleware;
    void* user_data;
    struct HttpMiddlewareNode* next;
} HttpMiddlewareNode;

// Forward declare these so the HttpServer struct can hold them as
// fields. Their full type definitions appear with the related
// public functions below.
struct HttpServer; /* forward, for hook signatures */
typedef void (*HttpLifecycleHook)(struct HttpServer* server, void* user_data);
typedef int  (*HttpReadyCheck)(void* user_data);

// Per-request observation hook (#260 Tier 3 — F1 logging, F2
// metrics). Fires once per completed request with the parsed
// request, the populated response, and the duration in
// microseconds. The hook may not mutate either argument; it's an
// observation point. Used by the built-in access logger and the
// per-route metrics collector. Multiple hooks may register; they
// run in registration order.
typedef void (*HttpRequestHook)(HttpRequest* req,
                                HttpServerResponse* res,
                                long duration_us,
                                void* user_data);

// HTTP Server
typedef struct HttpServer {
    int socket_fd;
    int port;
    char* host;
    int is_running;

    // Server-side TLS (#260 Tier 0). When tls_enabled == 1, every
    // accepted connection is wrapped in SSL_accept before the HTTP
    // parse begins. tls_ctx is an SSL_CTX*, declared here as void*
    // so this header doesn't pull in <openssl/ssl.h> for callers
    // that don't need it. Lazy-allocated by http_server_set_tls.
    int tls_enabled;
    void* tls_ctx;

    // HTTP/2 (#260 Tier 2). When h2_enabled == 1:
    //   - The TLS context advertises "h2" + "http/1.1" via ALPN so
    //     a TLS client picks h2 when it can; HTTP/1.1 stays the
    //     fallback for ALPN-unaware clients.
    //   - A plain (non-TLS) connection that sends `Upgrade: h2c`
    //     plus an HTTP2-Settings header is upgraded to HTTP/2 over
    //     cleartext (h2c) following RFC 7540 §3.2.
    //   - Per-connection h2 sessions wrap libnghttp2; each h2 stream
    //     dispatches into the same route table as HTTP/1.1, so
    //     middleware / metrics / access logs / health probes all
    //     apply uniformly.
    // Off by default; enable via http_server_set_h2_raw().
    int h2_enabled;
    int h2_max_concurrent_streams;  /* nghttp2 SETTINGS; 0 → 100 default */

    // Per-stream concurrent dispatch worker count. When > 0, the
    // stream-level dispatch runs on the shared std.worker pool,
    // sized to this many threads (aether_worker_pool_configure).
    // Every h2 connection on this server submits dispatch tasks
    // into that one pool, mirroring how the HTTP/1.1 connection
    // pool is shared across accepted connections. When 0 (default),
    // streams dispatch sequentially on the connection thread. Set
    // via http_server_set_h2_concurrent_dispatch_raw.
    int   h2_dispatch_workers;

    // Routing
    HttpRoute* routes;

    // Middleware
    HttpMiddlewareNode* middleware_chain;

    // Response transformers (#260 Tier 1). Run after the route
    // handler, before serialization. See
    // http_server_use_response_transformer above.
    struct HttpResponseTransformerNode* response_transformer_chain;

    // Lifecycle hooks (#260 Tier 3). NULL == not installed.
    HttpLifecycleHook on_start;
    void* on_start_user_data;
    HttpLifecycleHook on_stop;
    void* on_stop_user_data;

    // Health probes (#260 Tier 3). When non-zero, the route table
    // will already have routes registered for live/ready_path — the
    // health-probe handlers are stateless functions that close
    // over `ready_check` via the route's user_data slot.
    HttpReadyCheck ready_check;
    void* ready_check_user_data;

    // In-flight connection tracker for graceful shutdown (#260 Tier 3).
    // Incremented at the top of http_server_drain_connection and
    // decremented at the bottom; the shutdown helper waits on this.
    _Atomic int inflight_connections;

    // Per-request observation hook chain (#260 Tier 3 F1 / F2).
    // Hooks fire once per completed request with timing.
    struct HttpRequestHookNode* request_hook_chain;

    // SSE routes (#260 Tier 2). Separate from `routes` because they
    // dispatch differently — the handler owns the connection
    // lifetime and emits events directly to the wire.
    struct HttpSseRoute* sse_routes;

    // WebSocket routes (#260 Tier 2 / E2). Same dispatch shape as
    // SSE routes; the upgrade handshake runs first.
    struct HttpWsRoute* ws_routes;

    // Actor system
    Scheduler* scheduler;

    // Actor dispatch mode (opt-in: set via http_server_set_actor_handler)
    void* handler_actor;            // Legacy single actor (NULL = use C handlers)
    // Fire-and-forget: spawn one actor per request
    void (*send_fn)(void*, void*, size_t);      // aether_send_message (avoids link dep)
    void* (*spawn_fn)(int, void (*)(void*), size_t);  // scheduler_spawn_actor
    void (*release_fn)(void*);                  // scheduler_release_actor
    void (*step_fn)(void*);                     // User-provided step function for per-request actors

    // Configuration
    int max_connections;
    int keep_alive_timeout;          // legacy field (was always 30, never read)
    // HTTP/1.1 keep-alive (#260 Tier 0). When keep_alive_enabled is
    // non-zero, handle_client_connection loops over the same socket,
    // parsing successive requests until either:
    //   - the client sends Connection: close,
    //   - keep_alive_max requests have been served, or
    //   - keep_alive_idle_ms elapses with no new bytes on the socket.
    // The default is 0 (single-request, matches pre-#260 behaviour).
    int keep_alive_enabled;
    int keep_alive_max;              // 0 means "unlimited"
    int keep_alive_idle_ms;          // 0 means "use SO_RCVTIMEO default (30s)"

    // Keep-alive connection parking (#1663). When non-zero (the default),
    // a connection that stays alive between requests is handed to a poller
    // thread and its worker released, so the number of connections a server
    // can hold open bounds on file descriptors rather than on the worker
    // count (cores*2, capped at 64). Set to 0 to restore the pre-#1663
    // behaviour of a worker owning a connection for its whole life.
    int keep_alive_parking;

    // Worker pool for the non-actor accept path. Held here (rather than as
    // an accept-loop local) so a resumed connection can be handed back to a
    // worker from the park poller thread.
    struct HttpConnectionPool* conn_pool;

    // Accept-side I/O poller: wait for client data before dispatching to worker
    AetherIoPoller accept_poller;   // Platform poller for single-accept mode

    // Embedded/background mode: set by http_server_start_background_raw.
    // Quiets the interactive "Server running…/Press Ctrl+C" banner (an
    // embedded server is driven by http_server_stop, not a TTY) and makes
    // the detached accept/worker threads block async signals so they never
    // intercept a process-directed signal meant for the host application.
    // std-http-server-background-sigurg-poisons-harness.md
    int background;
    // Multi-accept: one accept thread per core with SO_REUSEPORT (opt-in)
    int multi_accept;               // 0 = single accept (default), 1 = SO_REUSEPORT multi-accept
    int accept_thread_count;
    pthread_t* accept_threads;      // Array of accept thread handles
    int* accept_listen_fds;         // Per-thread listen sockets (SO_REUSEPORT)
    AetherIoPoller* accept_pollers; // Per-thread I/O pollers
} HttpServer;

// ============================================================================
// Actor dispatch mode
// ============================================================================

// Message type IDs for HTTP actor dispatch
#define MSG_HTTP_REQUEST  200   // Pre-parsed request (legacy actor dispatch)
#define MSG_HTTP_CONNECTION 201 // Raw fd — actor does recv+parse+respond+close

// Legacy: pre-parsed request message (accept thread does recv+parse).
typedef struct {
    int type;               // MSG_HTTP_REQUEST (must be first field)
    int client_fd;          // Socket fd (actor writes response + closes)
    HttpRequest* request;   // Parsed request (actor must call http_request_free)
} HttpActorRequest;

// Full-actor mode: only the fd crosses thread boundary.
// The worker actor owns the entire lifecycle: recv, parse, respond, close.
typedef struct {
    int type;               // MSG_HTTP_CONNECTION (must be first field)
    int client_fd;          // Socket fd (actor owns everything)
} HttpConnectionMessage;

// Server lifecycle
HttpServer* http_server_create(int port);
int http_server_bind_raw(HttpServer* server, const char* host, int port);
// Set the bind host before server_start. Default is "0.0.0.0" (all
// interfaces). Pass "127.0.0.1" to bind loopback only — useful in
// tests because macOS / Windows firewalls don't prompt on loopback
// binds. No-op if `host` is NULL or empty.
void http_server_set_host(HttpServer* server, const char* host);
int http_server_start_raw(HttpServer* server);
int http_server_start_background_raw(HttpServer* server);
void http_server_stop(HttpServer* server);
void http_server_free(HttpServer* server);

// Enable HTTP/1.1 keep-alive on this server (#260 Tier 0). When
// `enabled` is non-zero, the connection handler reads multiple
// requests off one socket instead of closing after the first. The
// loop terminates when:
//   - the client sends `Connection: close` (or HTTP/1.0 without
//     `Connection: keep-alive`),
//   - the response status is one that mandates close (e.g. 408,
//     426),
//   - max_requests requests have been served on this socket
//     (0 = unlimited),
//   - idle_ms elapses with no new bytes (0 = use the default
//     30-second SO_RCVTIMEO already applied by the connection
//     handler).
// Returns "" on success; "server is null" on bad input.
const char* http_server_set_keepalive_raw(HttpServer* server,
                                          int enabled,
                                          int max_requests,
                                          int64_t idle_ns);

// Enable TLS termination on this server (#260 Tier 0). Loads the cert
// and private key from the given file paths (PEM-encoded), verifies
// they match, and configures the server's SSL_CTX. Returns "" on
// success; an error string on failure (file unreadable, parse error,
// cert/key mismatch, OpenSSL not built in). After this call, every
// accepted connection completes a TLS handshake before the HTTP parse
// begins; clients connecting plain-HTTP get rejected with a TLS
// handshake error. Idempotent — calling twice with the same files is
// a no-op success; calling with different files re-loads.
//
// When the build does not include OpenSSL (AETHER_HAS_OPENSSL undef),
// returns "TLS unavailable: built without OpenSSL".
const char* http_server_set_tls_raw(HttpServer* server,
                                    const char* cert_path,
                                    const char* key_path);

// Enable HTTP/2 (#260 Tier 2). When enabled:
//   - the TLS context advertises "h2" via ALPN (with "http/1.1" as
//     fallback) so clients pick HTTP/2 when both peers can speak it;
//   - plain (non-TLS) connections honour h2c upgrade requests
//     (RFC 7540 §3.2);
//   - per-stream dispatch reuses the existing route table, so all
//     middleware / metrics / access logs / health probes apply.
//
// `max_concurrent_streams` is the SETTINGS_MAX_CONCURRENT_STREAMS
// value advertised to peers. Pass 0 for libnghttp2's built-in
// default (100). Negative values return an error.
//
// When the build does not include libnghttp2
// (AETHER_HAS_NGHTTP2 undef), returns
// "HTTP/2 unavailable: built without libnghttp2".
const char* http_server_set_h2_raw(HttpServer* server,
                                   int max_concurrent_streams);

// Per-stream concurrent dispatch for HTTP/2. When `worker_count` > 0,
// each h2 connection spawns this many worker threads. Stream
// handlers run on those workers in parallel; the connection thread
// keeps reading frames + serialising responses. When 0 (default),
// streams dispatch sequentially on the connection thread.
//
// nghttp2_session is not thread-safe — only the connection thread
// calls into it. Workers receive the (request, response) pair, run
// the route handler, and post the result back; the connection
// thread submits the response on its own thread.
//
// Typical setting: 4–8 workers per connection. Excessively high
// values waste memory (each worker is a pthread); too low caps
// stream-level parallelism. Returns "" on success.
const char* http_server_set_h2_concurrent_dispatch_raw(HttpServer* server,
                                                       int worker_count);

// Routing
void http_server_add_route(HttpServer* server, const char* method, const char* path, HttpHandler handler, void* user_data);
void http_server_get(HttpServer* server, const char* path, HttpHandler handler, void* user_data);
void http_server_post(HttpServer* server, const char* path, HttpHandler handler, void* user_data);
void http_server_put(HttpServer* server, const char* path, HttpHandler handler, void* user_data);
void http_server_delete(HttpServer* server, const char* path, HttpHandler handler, void* user_data);

// Middleware
void http_server_use_middleware(HttpServer* server, HttpMiddleware middleware, void* user_data);

// Response-transformer chain (#260 Tier 1). Registered transformers
// run AFTER the route handler emits a response and BEFORE the
// response is serialized to the wire. Transformers may mutate the
// response (compress body, swap error pages, attach headers).
// Transformers run in registration order; each receives (req, res,
// user_data) and has no return value (mutation is the only effect).
typedef void (*HttpResponseTransformer)(HttpRequest* req,
                                        HttpServerResponse* res,
                                        void* user_data);
void http_server_use_response_transformer(HttpServer* server,
                                          HttpResponseTransformer xform,
                                          void* user_data);

// Request parsing
HttpRequest* http_parse_request(const char* raw_request);
// Length-aware sibling — buf need not be NUL-terminated and may contain
// embedded NUL bytes in the body. `len` is the authoritative byte count
// of the whole request slice (request line + headers + body). Required
// for binary bodies (Content-Encoding: x-lzf, octet-stream, etc.); the
// string-shaped sibling above is a thin wrapper around this for legacy
// callers that already have a NUL-terminated buffer.
HttpRequest* http_parse_request_n(const char* buf, size_t len);
const char* http_get_header(HttpRequest* req, const char* key);
const char* http_get_query_param(HttpRequest* req, const char* key);
const char* http_get_path_param(HttpRequest* req, const char* key);
void http_request_free(HttpRequest* req);

// Response building
HttpServerResponse* http_response_create();
void http_response_set_status(HttpServerResponse* res, int code);
void http_response_set_header(HttpServerResponse* res, const char* key, const char* value);
/* Append a header verbatim, even if a header with the same name
 * already exists. For protocols that allow/require repeated headers
 * (e.g. SVN's WebDAV uses 13+ `DAV:` headers per response). The
 * replace-on-duplicate behaviour of `set_header` collapses these. */
void http_response_add_header(HttpServerResponse* res, const char* key, const char* value);
/* Drop every header (including the defaults from http_response_create).
 * Used by record/replay frameworks that need to serve exactly what
 * was captured, with no Aether defaults overlaid. */
void http_response_clear_headers(HttpServerResponse* res);
void http_response_set_body(HttpServerResponse* res, const char* body);
/* Length-aware sibling — binary-safe set_body. Use when the body
 * may contain embedded NULs (binary content, gzip output, packed
 * binary, etc.); plain set_body uses strlen() and truncates at the
 * first NUL. */
void http_response_set_body_n(HttpServerResponse* res, const char* body, int length);
void http_response_json(HttpServerResponse* res, const char* json);
void* http_response_accept_tunnel(HttpServerResponse* res);
char* http_response_serialize(HttpServerResponse* res);  // caller must free()
// Length-aware variant — use this when the response body might contain
// binary (e.g. gzip-compressed). The returned buffer is NOT NUL-
// terminated as a string; pass *out_len bytes to send. Caller free()s.
char* http_response_serialize_len(HttpServerResponse* res, size_t* out_len);
void http_server_response_free(HttpServerResponse* res);

// Helpers
int http_route_matches(const char* pattern, const char* path, HttpRequest* req);
const char* http_status_text(int code);

// MIME type detection
const char* http_mime_type(const char* path);

// Static file serving
void http_serve_file(HttpServerResponse* res, const char* filepath);
void http_serve_static(HttpRequest* req, HttpServerResponse* res, void* base_dir);

// Actor dispatch mode (fire-and-forget, one actor per request)
// step_fn: actor step function that handles MSG_HTTP_REQUEST messages
// send_fn: pass aether_send_message
// spawn_fn: pass scheduler_spawn_actor
// release_fn: pass scheduler_release_actor
void http_server_set_actor_handler(HttpServer* server, void (*step_fn)(void*),
                                    void (*send_fn)(void*, void*, size_t),
                                    void* (*spawn_fn)(int, void (*)(void*), size_t),
                                    void (*release_fn)(void*));

// Graceful shutdown (#260 Tier 3). Stops accepting new connections,
// then waits up to `timeout_ns` for in-flight connections to finish
// naturally. Returns "" on clean drain, "timeout" if the deadline
// passed with connections still active. Callers typically install
// this on SIGTERM in their entry point. Sub-millisecond precision is
// rounded down internally to ms (the drain spin-wait is ms-granular).
const char* http_server_shutdown_graceful_raw(HttpServer* server, int64_t timeout_ns);

// Lifecycle hooks (#260 Tier 3). on_start fires once after the
// listen socket is bound but before the accept loop runs (good
// place to log "ready" or set readiness probes). on_stop fires
// after the accept loop exits but before sockets close (good place
// to flush logs, snapshot metrics). HttpLifecycleHook typedef
// declared earlier so the HttpServer struct could carry the field.
void http_server_set_on_start(HttpServer* server, HttpLifecycleHook hook, void* user_data);
void http_server_set_on_stop (HttpServer* server, HttpLifecycleHook hook, void* user_data);

// Health-probe endpoints (#260 Tier 3). Registers two routes:
//   `live_path`  always returns 200 "ok"  (process is up)
//   `ready_path` calls `ready_check` (returns 1 = 200, 0 = 503)
// Either path may be NULL/"" to skip registration. ready_check may
// be NULL — in that case `ready_path` always returns 200 too.
const char* http_server_set_health_probes_raw(HttpServer* server,
                                              const char* live_path,
                                              const char* ready_path,
                                              HttpReadyCheck ready_check,
                                              void* user_data);

// WebSocket (#260 Tier 2 / E2 — RFC 6455). Like SSE, the handler
// owns the connection lifetime: the server completes the upgrade
// handshake then hands control to the user. Inside the handler,
// ws_recv blocks for the next message (auto-handling ping/pong
// control frames internally); ws_send_text / ws_send_binary push
// outgoing messages. Returning from the handler closes the
// connection cleanly with a 1000 (Normal Closure) frame.
typedef struct HttpWsConn HttpWsConn;  /* opaque */
typedef void (*HttpWsHandler)(HttpRequest* req, HttpWsConn* ws, void* user_data);

void http_server_websocket(HttpServer* server, const char* path,
                           HttpWsHandler handler, void* user_data);

// Send a text frame (UTF-8). Returns 0 on success, -1 on transport
// error.
int http_ws_send_text(HttpWsConn* ws, const char* text);

// Send a binary frame. `len` is explicit so binary payloads with
// embedded NULs survive. Returns 0 on success, -1 on transport
// error.
int http_ws_send_binary(HttpWsConn* ws, const void* data, int len);

// Receive the next data frame (text or binary). Auto-handles
// fragmentation (assembles continuation frames) and control frames
// (responds to ping with pong, returns -1 on close).
//
// Returns:
//    1  — text frame received; data accessible via the helpers
//          below (NUL-terminated for text, raw bytes for binary)
//    2  — binary frame received; data via the helpers below
//   -1  — connection closed
//
// The message data lives in the connection's internal buffer and
// is valid until the next ws_recv / ws_send / ws_close call.
int http_ws_recv(HttpWsConn* ws);

// Accessors for the message read by the most recent ws_recv. Both
// safe to call after ws_recv returned 1 or 2; undefined behaviour
// otherwise.
const char* http_ws_message_data(HttpWsConn* ws);
int         http_ws_message_length(HttpWsConn* ws);

// Send a close frame with the given code (1000 = normal,
// 1001 = going away, 1002 = protocol error, 1008 = policy
// violation, 1011 = internal error) and an optional reason.
// Reason may be NULL/empty. Idempotent.
void http_ws_close(HttpWsConn* ws, int code, const char* reason);

// Server-Sent Events (#260 Tier 2). An SSE handler "owns" the
// connection — the server writes the initial 200 response with
// `Content-Type: text/event-stream`, then hands control to the
// handler which calls `http_sse_send_event` repeatedly to push
// events. The connection stays open until the handler returns or
// `http_sse_close` is called explicitly. Underlying transport is
// HTTP/1.1 with `Connection: close` (keep-alive doesn't apply —
// the response is open-ended, no Content-Length).
typedef struct HttpSseConn HttpSseConn;  /* opaque */
typedef void (*HttpSseHandler)(HttpRequest* req, HttpSseConn* sse, void* user_data);

void http_server_sse(HttpServer* server, const char* path,
                     HttpSseHandler handler, void* user_data);

// Send one event over the SSE connection. `event_name` may be NULL
// or "" to omit the `event:` line (browsers default-listen on the
// "message" event). Returns 0 on success, -1 on transport error
// (connection closed by peer); the handler should treat -1 as
// "stop emitting and return".
int http_sse_send_event(HttpSseConn* sse,
                        const char* event_name,
                        const char* data);

// Same as send_event but with an explicit event-id (browsers send
// it back via Last-Event-ID after a reconnect).
int http_sse_send_event_id(HttpSseConn* sse,
                           const char* event_name,
                           const char* data,
                           const char* id);

// Best-effort close. Idempotent. After this call, send_event
// returns -1.
void http_sse_close(HttpSseConn* sse);

// Register a per-request observation hook (#260 Tier 3). Multiple
// hooks may register; they run in registration order after the
// route handler emits the response and after any response
// transformers have run. Used by the built-in access logger and
// metrics collector below; user code can register custom hooks too.
void http_server_use_request_hook(HttpServer* server,
                                  HttpRequestHook hook,
                                  void* user_data);

// Built-in access logger (#260 Tier 3 / F1). Writes one log line
// per completed request. `format` selects the wire format:
//   "combined" — NCSA Combined Log Format (Apache default)
//   "json"     — one-object-per-line JSON
//   ""         — disabled
// `output_path` is a file path (opened with "ab" — appends so
// process restarts don't truncate), or "-" for stderr, or "" to
// disable. Returns "" on success, error string on failure.
const char* http_server_set_access_log_raw(HttpServer* server,
                                           const char* format,
                                           const char* output_path);

// Built-in per-route metrics collector (#260 Tier 3 / F2). Tracks
// request counts and latency histograms keyed by method+path-
// pattern. Exposes the standard Prometheus text format on a
// configurable endpoint (default "/metrics"). Returns "" on
// success, error string on failure.
const char* http_server_set_metrics_raw(HttpServer* server,
                                        const char* metrics_endpoint);

// Drain an accepted connection through the full HTTP lifecycle: TLS
// handshake (when enabled), parse request, run middleware, dispatch
// to the matching route, emit response, then loop for the next
// request when keep-alive is enabled. Closes the socket when the
// connection terminates. Idempotent on a closed/invalid fd
// (returns immediately).
//
// This is the building block that power callers — actor step
// functions, custom thread-pool workers, embedding hosts —
// use to plug a single connection into the standard server
// machinery. The thread-pool worker path inside this file calls it
// internally; user actor step functions registered via
// http_server_set_actor_handler should call it on the
// MSG_HTTP_CONNECTION message's client_fd to get the same
// keep-alive / TLS / route-dispatch behaviour. (#260 Tier 0)
void http_server_drain_connection(HttpServer* server, int client_fd);

// Resume a parked keep-alive connection on a worker thread (#1663).
// `parked_conn` is the opaque HttpConn* the park poller handed back.
// Takes ownership: the connection is closed and freed when it ends.
void http_server_resume_connection(HttpServer* server, void* parked_conn);

// Enable (1, the default) or disable (0) keep-alive connection parking.
// With parking off, a worker owns a connection for its whole life and the
// server can hold only as many connections as it has workers.
const char* http_server_set_keepalive_parking_raw(HttpServer* server, int enabled);

// Request accessors (for use from Aether .ae code via opaque ptr)
const char* http_request_method(HttpRequest* req);
const char* http_request_path(HttpRequest* req);
// Whole request body. On a STREAMING request (#644 v1 contract) the
// first call materializes: it drains the remaining wire bytes into a
// heap buffer once, so existing whole-body handlers keep working on
// large uploads at the cost of the O(Content-Length) allocation they
// asked for. Mixed usage — calling this after http_request_body_read
// already consumed part of the stream — returns "" (the prefix is
// gone; a silent tail-as-whole would corrupt).
const char* http_request_body(HttpRequest* req);
// Sibling of http_request_body — returns the body's byte length so
// callers can read NUL-bearing payloads (svn PUT, image uploads,
// gzip-compressed bodies) without strlen() truncating at the first
// embedded zero. Returns 0 if req is NULL or has no body. On a
// streaming request this is the declared Content-Length until the
// body is materialized, then the actual received count.
int         http_request_body_length(HttpRequest* req);

// #626 / #644 — Chunked-read request body. TLS split-accessor: the
// bytes live in a per-thread buffer until the next call or explicit
// release. Two backing modes: bodies that fit one connection buffer
// (16 KiB) are pre-buffered and served as random-access slices; larger
// bodies are STREAMED — the dispatcher fires the handler at headers-
// complete and each read pulls the next window straight off the
// socket (sequential offsets only), so peak RAM is O(buf + window)
// per connection. Backpressure is the TCP flow control itself: the
// server simply doesn't recv until the handler asks for more.
// Transfer-Encoding: chunked request bodies remain unsupported (as
// before the reshape): with no Content-Length the body length is 0
// and no body is read — the deliberate v1 semantics decision of #644.
int         http_request_body_read_raw(HttpRequest* req, int offset, int max);
int         http_request_body_complete(HttpRequest* req);
const char* http_get_request_body_read(void);
int         http_get_request_body_read_length(void);
void        http_release_request_body_read(void);

const char* http_request_query(HttpRequest* req);

// Trusted TCP peer address — the IP `getpeername(2)` reports for the
// connection that carried this request. Use this (not the
// `X-Forwarded-For` header) for in-app source-IP allow/deny lists on
// a directly-exposed listener: clients can set any value they like
// into the header, but only the kernel's view of the peer reflects
// reality. Returns the dotted-quad form for IPv4 ("203.0.113.7") and
// the `inet_ntop` AF_INET6 form for IPv6 ("2001:db8::1") — no
// brackets, no scope id. Returns "" when unavailable (Unix-domain
// socket, getpeername failed, or req is NULL). The companion
// X-Forwarded-For middleware remains the right tool when this server
// sits behind a trusted reverse proxy.
const char* http_request_remote_addr(HttpRequest* req);
// Companion port for `http_request_remote_addr`. Returns 0 when
// unavailable (same conditions as the address).
int         http_request_remote_port(HttpRequest* req);

// Server-side socket name — what `getsockname(2)` reports for the
// accepted fd. Needed when the listener binds 0.0.0.0 and the
// handler must learn which NIC actually received the request:
// multi-NIC / multi-tenant per-IP routing, "admin endpoint from
// internal NIC only". Same string format and unavailability
// semantics as the remote-* pair.
const char* http_request_local_addr(HttpRequest* req);
int         http_request_local_port(HttpRequest* req);

// Transport scheme. `http_request_scheme` is "https" when this
// connection was TLS-wrapped (ALPN or h2c upgrade transparent —
// what mattered is whether the bytes were over TLS), else "http".
// `http_request_is_tls` exposes the same as a 0/1 int for callers
// that want a bool without the string compare. Drives canonical-
// URL building, redirect targets, cookie Secure-flag decisions.
const char* http_request_scheme(HttpRequest* req);
int         http_request_is_tls(HttpRequest* req);

// HTTP version as the parser captured it on the request line —
// "HTTP/1.0", "HTTP/1.1", or "HTTP/2.0". h2 connections always
// surface as "HTTP/2.0" regardless of the inner stream wire form.
// Returns "" when unset (parse failed before the version slot).
const char* http_request_http_version(HttpRequest* req);

// Request-header iteration — enumerate every received header (named
// http_get_header only does a single lookup). Wire order, duplicates
// preserved, no sort/dedup. index in [0, http_request_header_count);
// out-of-range name/value return "". 50-header cap, as the parser.
int         http_request_header_count(HttpRequest* req);
const char* http_request_header_name(HttpRequest* req, int index);
const char* http_request_header_value(HttpRequest* req, int index);

// Format a Common/Combined Log Format timestamp: "DD/MMM/YYYY:HH:MM:SS +0000".
// The month is always the English three-letter abbreviation, never the
// locale's — CLF is a parseable interchange format. Exposed (rather than
// static) so the locale-independence can be tested directly.
void http_format_clf_time(char* out, size_t out_size, const struct tm* tmv);

#endif
