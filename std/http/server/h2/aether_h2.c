/* aether_h2.c — HTTP/2 server-side glue, libnghttp2 wrapper.
 *
 * Issue #260 Tier 2.
 *
 * Layout:
 *   - Stream state lives in AetherH2Stream nodes hanging off a
 *     per-session linked list; nghttp2 owns its own stream metadata
 *     internally, ours holds the "what we're going to dispatch" view
 *     (request bits assembled so far, per-stream DATA payload, etc.).
 *   - The nghttp2 callbacks are intentionally narrow shims that
 *     append to / consume from those Aether-side structs. The
 *     dispatch decision (call into the existing route table) only
 *     fires once nghttp2 reports END_STREAM on the request side.
 *   - Response submission goes back through nghttp2 via
 *     nghttp2_submit_response + a data provider that reads from the
 *     stream's response-body buffer.
 *
 * When the build doesn't link libnghttp2, every public function is
 * a stub that returns the "unavailable" sentinel. The compile gate
 * is AETHER_HAS_NGHTTP2.
 */

#include "aether_h2.h"
#include "../../../mem/aether_grow.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* The monotonic clock streams are stamped with, in ms (#3738). */
int64_t aether_h2_now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return (int64_t)time(NULL) * 1000;
}

#ifdef AETHER_HAS_NGHTTP2

#include <nghttp2/nghttp2.h>
#include "../../../net/aether_http_server.h"
#include "../../../cryptography/aether_cryptography.h"

/* Per-stream concurrent dispatch (#260 follow-up) is POSIX-only.
 * The worker pool relies on pipe(2) + poll(2) for cross-thread
 * wake-ups; Windows lacks a clean equivalent for sockets+pipes in
 * one poll set, so on Windows the pool path compiles out and h2
 * streams dispatch sequentially on the connection thread (which
 * matches the original v1 behaviour). */
#if !defined(_WIN32)
#define AETHER_H2_HAS_POOL 1
/* The portable thread shim, not raw <pthread.h> (see its header note).
 * It is real pthreads on POSIX, a CRITICAL_SECTION shim on Windows, and
 * no-op stubs where AETHER_HAS_THREADS is 0. Including <pthread.h> directly
 * broke wasm32-wasi (#1655): wasi-libc SHIPS a pthread.h even though its
 * pthread_create is a stub, so its real typedefs collided with the
 * threadless shim's in the same TU. */
#include "../../../../runtime/utils/aether_thread.h"
#include "../../../../runtime/utils/aether_spawn.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "../../../worker/aether_worker.h"
#include <time.h>
#endif

/* Per-stream state — what we've collected on the request side and
 * what we owe back on the response side. Streams live in a
 * singly-linked list off the session; lookup is O(N) which is fine
 * for the modest number of in-flight streams a connection may hold: the
 * server advertises SETTINGS_MAX_CONCURRENT_STREAMS, 100 unless set
 * (HTTP_H2_DEFAULT_MAX_CONCURRENT_STREAMS, #3738). */
typedef struct AetherH2Stream {
    int32_t stream_id;

    /* Request side — assembled from on_header callbacks until the
     * client end-stream signals "ready to dispatch." Everything is
     * heap-allocated since nghttp2 doesn't retain header buffers
     * past the callback that delivered them. */
    char*  method;       /* :method pseudo-header */
    char*  path;         /* :path pseudo-header */
    char*  scheme;       /* :scheme pseudo-header (informational) */
    char*  authority;    /* :authority pseudo-header (Host equivalent) */
    char*  header_block; /* HTTP/1.1-shaped header block we hand to
                          * the existing route handler so middleware
                          * that introspects raw headers (CORS,
                          * basic-auth, vhost) keeps working. */
    size_t header_block_len;
    size_t header_block_cap;
    char*  body;
    size_t body_len;
    size_t body_cap;
    int    request_done;   /* peer sent END_STREAM on the request */
    /* Set when the body passed the server's max_body and the stream was
     * reset (#3738): later DATA is dropped and the stream never dispatched. */
    int    body_refused;
    /* When the stream's first HEADERS arrived, in aether_h2_now_ms's
     * clock: its request must be whole by the deadline that starts here
     * (aether_h2_session_expire, #3738). */
    int64_t opened_ms;
    /* The request's deadline as it stands, 0 until first worked out, and
     * the body bytes that have extended it so far: the minimum rate's credit
     * is held to the rate window (http_body_budget_credit). */
    int64_t due_ms;
    size_t  body_credited;
    /* The request's header list so far, counted as
     * SETTINGS_MAX_HEADER_LIST_SIZE counts one (each field's name and value
     * and 32), and whether it passed the server's h2_max_header_list: the
     * stream is then answered 431 and nothing more of it is kept. */
    size_t header_list;
    int    header_refused;

    /* Response side — populated when the route handler returns,
     * consumed by the nghttp2_data_source_read_callback as the
     * library drains the response onto the wire. */
    int          response_status;
    char*        response_body;
    size_t       response_body_len;
    size_t       response_body_cursor;
    /* Header pairs ready to submit. Each entry is a malloc'd name+
     * value pair owned by the stream. */
    nghttp2_nv*  response_headers;
    size_t       response_header_count;
    int          response_submitted;

    struct AetherH2Stream* next;
} AetherH2Stream;

/* Concurrent dispatch architecture (#260 follow-up).
 *
 * Stream handlers run on the shared std.worker pool: one bounded set
 * of reusable threads (size defaults to the core count) serves every
 * h2 connection on every server in the process. This mirrors the
 * HTTP/1.1 connection pool (a fixed thread budget that does not scale
 * with concurrent client count) and goes one better: a per-connection
 * pool would spawn N threads per h2 connection, so 1000 keep-alive
 * clients would mean thousands of threads. One process-wide pool keeps
 * the OS thread count constant.
 *
 * A pool thread, not an actor: HTTP route handlers call into arbitrary
 * blocking C/Aether code (sleep, fs read, db query, upstream http
 * call). Aether actors are cooperative, so a blocking actor
 * monopolises its scheduler thread until it returns; with only a few
 * scheduler threads, blocking handlers would starve every other actor.
 * A pool thread absorbs the block: it ties up one OS thread while the
 * kernel keeps the rest of the system responsive.
 *
 * Per-session state (ready queue + wake pipe) stays local because the
 * connection thread serialising responses is per-connection; targeted
 * wake-up is essential so a task finishing on connection A does not
 * wake connection B's poll loop. */
typedef struct H2DispatchTask {
    /* The session this task belongs to. Workers call back through
     * this pointer to push the result onto the session's ready
     * queue and wake its connection thread. */
    struct AetherH2Session* session;
    AetherH2Stream*         stream;
    HttpRequest*            req;
    HttpServerResponse*     res;
    struct H2DispatchTask*  next;
} H2DispatchTask;

struct AetherH2Session {
    nghttp2_session* ng;
    HttpServer*      server;
    void*            conn_userdata;

    /* Pending bytes nghttp2 has already serialised but the wire
     * callback hasn't accepted yet. We pull from nghttp2's send
     * pump on every drain call; this buffer absorbs short-write
     * cases where the wire callback can only take part of a chunk. */
    uint8_t*         out_buf;
    size_t           out_buf_len;
    size_t           out_buf_cap;

    AetherH2Stream*  streams;
    int              fatal;   /* set when a callback flagged a hard error */
    /* The request body bytes the streams hold, not yet handed to a handler:
     * no more than the server's max_body on one connection (#3738), as one
     * HTTP/1.1 connection holds one body at a time. */
    size_t           body_held;
    /* When a stream last opened or closed (aether_h2_now_ms): the start of
     * the connection's idle time once none is open (#3738). A request
     * answered within one turn of the connection's loop is never seen open
     * there. */
    int64_t          last_active_ms;
    /* When a response was last submitted or bytes last went to the client
     * (aether_h2_session_last_progress). */
    int64_t          last_progress_ms;

#ifdef AETHER_H2_HAS_POOL
    /* Per-session async-dispatch state. The worker pool is the shared
     * std.worker pool; only the wake-up + ready-queue plumbing is per
     * connection (so a task finishing on connection A doesn't wake up
     * connection B's poll loop). */
    int              dispatch_enabled;     /* mirrors server->h2_dispatch_workers > 0 */
    pthread_mutex_t  ready_mu;
    pthread_cond_t   ready_cv;             /* signaled when in_flight drops to 0 */
    H2DispatchTask*  ready_head;
    H2DispatchTask*  ready_tail;
    int              wake_fd_read;
    int              wake_fd_write;
    int              in_flight;            /* tasks enqueued but not yet
                                            * submitted to nghttp2; the
                                            * connection thread can't tear
                                            * down the session while > 0 */
#endif
};

/* ------------------------------------------------------------------
 * Stream bookkeeping
 * ------------------------------------------------------------------ */

static AetherH2Stream* stream_lookup(AetherH2Session* s, int32_t id) {
    for (AetherH2Stream* str = s->streams; str; str = str->next) {
        if (str->stream_id == id) return str;
    }
    return NULL;
}

static AetherH2Stream* stream_get_or_create(AetherH2Session* s, int32_t id) {
    AetherH2Stream* str = stream_lookup(s, id);
    if (str) return str;
    str = calloc(1, sizeof(*str));
    if (!str) return NULL;
    str->stream_id = id;
    str->opened_ms = aether_h2_now_ms();
    s->last_active_ms = str->opened_ms;
    str->next = s->streams;
    s->streams = str;
    return str;
}

static void stream_free(AetherH2Stream* str) {
    if (!str) return;
    free(str->method);
    free(str->path);
    free(str->scheme);
    free(str->authority);
    free(str->header_block);
    free(str->body);
    free(str->response_body);
    if (str->response_headers) {
        for (size_t i = 0; i < str->response_header_count; i++) {
            free(str->response_headers[i].name);
            free(str->response_headers[i].value);
        }
        free(str->response_headers);
    }
    free(str);
}

static void stream_unlink(AetherH2Session* s, int32_t id) {
    AetherH2Stream** cur = &s->streams;
    while (*cur) {
        if ((*cur)->stream_id == id) {
            AetherH2Stream* dead = *cur;
            *cur = dead->next;
            s->body_held -= dead->body_len <= s->body_held ? dead->body_len : s->body_held;
            s->last_active_ms = aether_h2_now_ms();
            stream_free(dead);
            return;
        }
        cur = &(*cur)->next;
    }
}

/* Append `data` (of `len` bytes) to a heap buffer that grows by
 * doubling. Returns 0 on success, -1 on OOM. */
static int buf_append(char** buf, size_t* len, size_t* cap,
                      const char* data, size_t data_len) {
    if (*len + data_len + 1 > *cap) {
        size_t new_cap = aether_buf_grow_capacity(*cap, 256,
                                                  *len + data_len + 1, 1);
        if (!new_cap) return -1;
        char* nb = realloc(*buf, new_cap);
        if (!nb) return -1;
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    (*buf)[*len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------
 * nghttp2 callbacks
 * ------------------------------------------------------------------ */

static ssize_t ng_send_callback(nghttp2_session* ng,
                                const uint8_t* data, size_t len,
                                int flags, void* user_data) {
    (void)ng; (void)flags;
    AetherH2Session* s = (AetherH2Session*)user_data;
    /* Buffer everything; the drain function flushes on its own
     * schedule. Returning a short value here is interpreted as
     * NGHTTP2_ERR_WOULDBLOCK, which the library handles gracefully. */
    if (s->out_buf_len + len > s->out_buf_cap) {
        size_t new_cap = (s->out_buf_cap == 0) ? 4096 : s->out_buf_cap;
        while (new_cap < s->out_buf_len + len) new_cap *= 2;
        uint8_t* nb = realloc(s->out_buf, new_cap);
        if (!nb) return NGHTTP2_ERR_CALLBACK_FAILURE;
        s->out_buf = nb;
        s->out_buf_cap = new_cap;
    }
    memcpy(s->out_buf + s->out_buf_len, data, len);
    s->out_buf_len += len;
    return (ssize_t)len;
}

/* RFC 7540 §8.1.2.2 — connection-specific header fields are
 * forbidden in HTTP/2 requests AND responses. Receiving any of
 * these on a request is a protocol violation; we send RST_STREAM
 * with PROTOCOL_ERROR. (Response-side filtering happens during
 * dispatch_stream — we already strip these.) */
static int header_is_forbidden_in_h2(const uint8_t* name, size_t namelen) {
    static const struct { const char* n; size_t l; } banned[] = {
        { "connection",         10 },
        { "transfer-encoding",  17 },
        { "upgrade",             7 },
        { "keep-alive",         10 },
        { "proxy-connection",   16 },
        { NULL, 0 }
    };
    for (int i = 0; banned[i].n; i++) {
        if (namelen == banned[i].l &&
            memcmp(name, banned[i].n, namelen) == 0) {
            return 1;
        }
    }
    /* `te` is allowed but only with the value "trailers" per
     * §8.1.2.2. Caller checks the value separately when name=="te". */
    return 0;
}

/* Called for every header name/value pair on a request stream.
 * We collect the four pseudo-headers separately (since they have
 * special meaning) and append everything else into a header block
 * that mimics the HTTP/1.1 wire format — that way middleware that
 * pattern-matches the raw header block (CORS, basic-auth, vhost)
 * keeps working without an h2-specific code path. */
static int ng_on_header_callback(nghttp2_session* ng,
                                 const nghttp2_frame* frame,
                                 const uint8_t* name, size_t namelen,
                                 const uint8_t* value, size_t valuelen,
                                 uint8_t flags, void* user_data) {
    (void)ng; (void)flags;
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    AetherH2Session* s = (AetherH2Session*)user_data;
    AetherH2Stream* str = stream_get_or_create(s, frame->hd.stream_id);
    if (!str) return NGHTTP2_ERR_CALLBACK_FAILURE;

    /* The header list is bounded as an HTTP/1.1 head is (#3738). HPACK
     * lets a few bytes on the wire stand for a field of up to the dynamic
     * table's size, over and over, and nghttp2 bounds neither the fields
     * nor their total, so a HEADERS frame and its CONTINUATIONs grew this
     * stream's header block without limit. Past h2_max_header_list the
     * stream is answered 431 once its HEADERS end (ng_on_frame_recv) and
     * nothing more of the list is kept. */
    if (str->header_refused) return 0;
    size_t list_max = s->server && s->server->limits.h2_max_header_list > 0
                    ? (size_t)s->server->limits.h2_max_header_list
                    : (size_t)HTTP_H2_DEFAULT_MAX_HEADER_LIST;
    str->header_list += namelen + valuelen + 32;
    if (str->header_list > list_max) {
        str->header_refused = 1;
        free(str->header_block);
        str->header_block = NULL;
        str->header_block_len = str->header_block_cap = 0;
        return 0;
    }

    /* Reject connection-specific request headers before they reach
     * the request shape — a peer that sent these is malformed and
     * the stream MUST be rejected (RFC 7540 §8.1.2.2). The TE
     * header is conditional: the only allowed value is "trailers". */
    if (namelen > 0 && name[0] != ':') {
        if (header_is_forbidden_in_h2(name, namelen)) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        if (namelen == 2 && memcmp(name, "te", 2) == 0) {
            if (!(valuelen == 8 && memcmp(value, "trailers", 8) == 0)) {
                return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
            }
        }
    }

    /* Pseudo-headers begin with ':'. Normal headers are lowercase
     * per RFC 7540 §8.1.2. */
    if (namelen > 0 && name[0] == ':') {
        char** slot = NULL;
        if (namelen == 7 && memcmp(name, ":method", 7) == 0)        slot = &str->method;
        else if (namelen == 5 && memcmp(name, ":path", 5) == 0)     slot = &str->path;
        else if (namelen == 7 && memcmp(name, ":scheme", 7) == 0)   slot = &str->scheme;
        else if (namelen == 10 && memcmp(name, ":authority", 10) == 0) slot = &str->authority;
        if (slot) {
            free(*slot);
            *slot = malloc(valuelen + 1);
            if (!*slot) return NGHTTP2_ERR_CALLBACK_FAILURE;
            memcpy(*slot, value, valuelen);
            (*slot)[valuelen] = '\0';
        }
        return 0;
    }

    /* Regular header — append `Name: value\r\n` to the header block.
     * h2 names are lowercase by spec; we emit them as-is, which most
     * existing middleware handles via case-insensitive comparison. */
    if (buf_append(&str->header_block, &str->header_block_len,
                   &str->header_block_cap,
                   (const char*)name, namelen) != 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (buf_append(&str->header_block, &str->header_block_len,
                   &str->header_block_cap, ": ", 2) != 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (buf_append(&str->header_block, &str->header_block_len,
                   &str->header_block_cap,
                   (const char*)value, valuelen) != 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (buf_append(&str->header_block, &str->header_block_len,
                   &str->header_block_cap, "\r\n", 2) != 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    return 0;
}

/* DATA frame chunks land here; we just accumulate the body until
 * END_STREAM. The body buffer doubles, so amortised append cost is
 * constant; nghttp2 already enforces SETTINGS_MAX_FRAME_SIZE so a
 * single chunk can't blow the buffer arbitrarily. */
static int ng_on_data_chunk_recv(nghttp2_session* ng,
                                 uint8_t flags,
                                 int32_t stream_id,
                                 const uint8_t* data, size_t len,
                                 void* user_data) {
    (void)ng; (void)flags;
    AetherH2Session* s = (AetherH2Session*)user_data;
    AetherH2Stream* str = stream_lookup(s, stream_id);
    /* A stream already answered (refused, or past its deadline) keeps none
     * of what it still sends. */
    if (!str || str->body_refused || str->response_submitted) return 0;
    /* Every h2 request body is held whole until END_STREAM, and nghttp2's
     * automatic WINDOW_UPDATE kept it flowing with no bound (#3738). A body
     * past the server's limit (the one request_body applies, #3740) resets
     * the stream with CANCEL rather than REFUSED_STREAM, which tells a
     * client the request may be retried as it was. What it sent is freed
     * and the stream is never dispatched. */
    int64_t max_body = (s->server && s->server->limits.max_body > 0)
                     ? s->server->limits.max_body : HTTP_DEFAULT_MAX_BODY;
    /* The limit holds for the connection as a whole too: each of its up
     * to 100 streams could hold a body of max_body at once, a hundred
     * times what one HTTP/1.1 connection can make the server hold. A
     * stream that would take the connection past it is reset with
     * REFUSED_STREAM, which tells the client the request was not
     * processed and may be sent again, once the others are done. */
    int too_big = (uint64_t)str->body_len + (uint64_t)len > (uint64_t)max_body;
    int too_much = !too_big &&
                   (uint64_t)s->body_held + (uint64_t)len > (uint64_t)max_body;
    if (too_big || too_much) {
        s->body_held -= str->body_len <= s->body_held ? str->body_len : s->body_held;
        free(str->body);
        str->body = NULL;
        str->body_len = str->body_cap = 0;
        str->body_refused = 1;
        str->response_submitted = 1;
        nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE, stream_id,
                                  too_big ? NGHTTP2_CANCEL : NGHTTP2_REFUSED_STREAM);
        return 0;
    }
    if (buf_append(&str->body, &str->body_len, &str->body_cap,
                   (const char*)data, len) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    s->body_held += len;
    return 0;
}

/* nghttp2_data_source_read_callback for response body streaming.
 * We hand back chunks from the stream's response_body buffer; when
 * the cursor catches up to the buffer length we set END_STREAM. */
static ssize_t ng_response_data_read_callback(nghttp2_session* ng,
                                              int32_t stream_id,
                                              uint8_t* buf, size_t length,
                                              uint32_t* data_flags,
                                              nghttp2_data_source* source,
                                              void* user_data) {
    (void)ng; (void)source;
    AetherH2Session* s = (AetherH2Session*)user_data;
    AetherH2Stream* str = stream_lookup(s, stream_id);
    if (!str) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    size_t remaining = (str->response_body_len > str->response_body_cursor)
                        ? str->response_body_len - str->response_body_cursor
                        : 0;
    size_t n = (remaining < length) ? remaining : length;
    if (n > 0 && str->response_body) {
        memcpy(buf, str->response_body + str->response_body_cursor, n);
        str->response_body_cursor += n;
    }
    if (str->response_body_cursor >= str->response_body_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t)n;
}

/* Forward decl — defined further below so it can call into the
 * existing route table without exposing the dispatch shape via a
 * header circular include. */
static void dispatch_stream(AetherH2Session* s, AetherH2Stream* str);

/* on_frame_recv — called once per fully-received frame. The frames
 * we care about are HEADERS-with-END_STREAM (request fully buffered,
 * dispatch to handler) and DATA-with-END_STREAM (same, when the
 * request has a body). */
static int ng_on_frame_recv_callback(nghttp2_session* ng,
                                     const nghttp2_frame* frame,
                                     void* user_data) {
    (void)ng;
    AetherH2Session* s = (AetherH2Session*)user_data;
    int end = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) ? 1 : 0;
    int is_request_done = 0;

    /* A request whose header list passed the limit (ng_on_header_callback)
     * is answered 431 once its header block ends, as an HTTP/1.1 head past
     * its limit is, and then reset (NO_ERROR: the answer is complete). It
     * is never dispatched, and what it sends after is dropped. */
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        AetherH2Stream* str = stream_lookup(s, frame->hd.stream_id);
        if (str && str->header_refused && !str->response_submitted) {
            static const char status_name[] = ":status";
            static const char status_431[] = "431";
            nghttp2_nv nv = { (uint8_t*)status_name, (uint8_t*)status_431,
                              sizeof(status_name) - 1, sizeof(status_431) - 1,
                              NGHTTP2_NV_FLAG_NONE };
            str->response_submitted = 1;
            nghttp2_submit_response(s->ng, frame->hd.stream_id, &nv, 1, NULL);
            if (!end)
                nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                          frame->hd.stream_id, NGHTTP2_NO_ERROR);
            return 0;
        }
    }

    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST &&
        end) {
        is_request_done = 1;
    } else if (frame->hd.type == NGHTTP2_DATA && end) {
        is_request_done = 1;
    }

    if (is_request_done) {
        AetherH2Stream* str = stream_lookup(s, frame->hd.stream_id);
        if (str && !str->request_done) {
            str->request_done = 1;
            dispatch_stream(s, str);
        }
    }
    return 0;
}

static int ng_on_stream_close(nghttp2_session* ng,
                              int32_t stream_id, uint32_t error_code,
                              void* user_data) {
    (void)ng; (void)error_code;
    AetherH2Session* s = (AetherH2Session*)user_data;
    stream_unlink(s, stream_id);
    return 0;
}

/* ------------------------------------------------------------------
 * Stream dispatch — bridge to the existing route table.
 *
 * We synthesise the same shape an HTTP/1.1 connection would have
 * delivered: an HttpRequest with method/path/headers/body, and an
 * HttpServerResponse the route handler can populate. After the
 * handler returns, we extract its status + body + headers and submit
 * them back through nghttp2.
 *
 * The shared dispatch helper lives in the main server file so the
 * HTTP/1.1 and HTTP/2 paths exercise the same middleware chain,
 * route lookup, transformer chain, and request hooks.
 * ------------------------------------------------------------------ */

/* Shared dispatcher: runs middleware → route lookup → handler →
 * response transformers → request hooks, mutating `res` in place.
 * Defined in aether_http_server.c. */
extern void http_server_dispatch_for_h2(HttpServer* server,
                                        HttpRequest* req,
                                        HttpServerResponse* res);

static char* dup_str(const char* src) {
    if (!src) return NULL;
    size_t n = strlen(src) + 1;
    char* d = malloc(n);
    if (!d) return NULL;
    memcpy(d, src, n);
    return d;
}

/* Build an HttpRequest from the h2 stream's collected pieces. The
 * struct's public fields (method, path, header_keys/values, body)
 * mirror what HTTP/1.1 would have produced — no h2-specific code
 * path needed downstream. We parse the simple `Name: value\r\n`
 * header block we accumulated in on_header_callback. */
static HttpRequest* request_from_stream(AetherH2Stream* str) {
    HttpRequest* req = calloc(1, sizeof(HttpRequest));
    if (!req) return NULL;
    req->method        = dup_str(str->method ? str->method : "GET");
    /* method is dereferenced by route dispatch (strcmp); a NULL here would be a
     * delayed crash far from this alloc, so fail the request build now. */
    if (!req->method) { free(req); return NULL; }
    /* Per RFC 7540, the :path pseudo-header is the request-target
     * which embeds the query string. The HTTP/1.1 parser splits at
     * '?' to populate path + query_string separately, and downstream
     * code (route matching, http_get_query_param, observability
     * hooks) depends on that split. Mirror it here so h2 streams
     * present the same shape. */
    {
        const char* full_path = str->path ? str->path : "/";
        const char* qmark = strchr(full_path, '?');
        if (qmark) {
            size_t plen = (size_t)(qmark - full_path);
            req->path = malloc(plen + 1);
            if (req->path) {
                memcpy(req->path, full_path, plen);
                req->path[plen] = '\0';
            }
            req->query_string = dup_str(qmark + 1);
        } else {
            req->path = dup_str(full_path);
            req->query_string = dup_str("");
        }
        if (!req->path) req->path = dup_str("/");
    }
    req->http_version  = dup_str("HTTP/2");

    /* Count headers in the block to size the parallel arrays. */
    int n = 0;
    if (str->header_block) {
        const char* p = str->header_block;
        while (*p) {
            const char* eol = strstr(p, "\r\n");
            if (!eol) break;
            if (eol != p) n++;
            p = eol + 2;
        }
    }
    if (n > 0) {
        req->header_keys   = calloc((size_t)n, sizeof(char*));
        req->header_values = calloc((size_t)n, sizeof(char*));
        if (!req->header_keys || !req->header_values) {
            free(req->header_keys); free(req->header_values);
            free(req->method); free(req->path);
            free(req->query_string); free(req->http_version);
            free(req);
            return NULL;
        }
        /* Recorded, so an edit grows the arrays from here (#3234). */
        req->header_cap = n;
        const char* p = str->header_block;
        int i = 0;
        while (*p && i < n) {
            const char* eol = strstr(p, "\r\n");
            if (!eol) break;
            const char* colon = memchr(p, ':', (size_t)(eol - p));
            if (colon && colon > p) {
                size_t klen = (size_t)(colon - p);
                const char* vstart = colon + 1;
                while (vstart < eol && (*vstart == ' ' || *vstart == '\t')) vstart++;
                size_t vlen = (size_t)(eol - vstart);
                req->header_keys[i]   = malloc(klen + 1);
                req->header_values[i] = malloc(vlen + 1);
                if (!req->header_keys[i] || !req->header_values[i]) {
                    /* One of the pair failed: free the other so it doesn't leak
                     * (header_count won't cover this un-incremented slot). */
                    free(req->header_keys[i]);   req->header_keys[i] = NULL;
                    free(req->header_values[i]); req->header_values[i] = NULL;
                    p = eol + 2; continue;
                }
                memcpy(req->header_keys[i], p, klen);
                req->header_keys[i][klen] = '\0';
                memcpy(req->header_values[i], vstart, vlen);
                req->header_values[i][vlen] = '\0';
                i++;
            }
            p = eol + 2;
        }
        req->header_count = i;
    }

    /* The body moves to the request rather than being copied (#3738): a
     * copy held it twice, the stream's until the stream closed, up to
     * twice max_body for one request. buf_append keeps it NUL-terminated,
     * as a request body is. */
    if (str->body && str->body_len > 0) {
        req->body = str->body;
        req->body_length = str->body_len;
        str->body = NULL;
        str->body_len = str->body_cap = 0;
    }
    return req;
}

/* Forward decl — defined in aether_http_server.c. */
extern void http_request_free(HttpRequest* req);

#ifdef AETHER_H2_HAS_POOL
/* Runs one dispatch task on the shared std.worker pool. The task carries the
 * session it belongs to, so we know which ready queue + wake pipe to push to.
 *
 * CRITICAL: nghttp2_session is single-threaded, so it is NEVER touched here;
 * only the connection thread calls into the library. Touching it off-thread
 * would corrupt the session and crash the connection. The handler reads from
 * `req` (immutable after this point) and writes to `res`. */
static void h2_task_run(void* arg) {
    H2DispatchTask* task = (H2DispatchTask*)arg;
    task->next = NULL;
    AetherH2Session* sess = task->session;

    /* Route table + middleware are read-only after startup, so running the
     * handler here (off the connection thread) is safe across any number of
     * sessions; nghttp2_session is NEVER touched here. Each task owns its
     * own (req, res). The worker is one of the server's own threads for the
     * handler: its connection thread waits for this task, so a stop from the
     * handler must not wait for the run. */
    http_server_thread_enter(sess->server);
    http_server_dispatch_for_h2(sess->server, task->req, task->res);
    http_server_thread_leave(sess->server);

    /* Push to the session's ready queue + wake its connection thread. The
     * session's in_flight counter (bumped before submit) keeps `sess` alive
     * until the connection thread drains this task. */
    pthread_mutex_lock(&sess->ready_mu);
    if (sess->ready_tail) {
        sess->ready_tail->next = task;
        sess->ready_tail = task;
    } else {
        sess->ready_head = sess->ready_tail = task;
    }
    pthread_mutex_unlock(&sess->ready_mu);

    char wake_byte = 1;
    ssize_t w;
    do {
        w = write(sess->wake_fd_write, &wake_byte, 1);
    } while (w < 0 && errno == EINTR);
    /* EAGAIN is fine: the pipe is full, so a wake-up is already pending. */
}

static void task_free(H2DispatchTask* t) {
    if (!t) return;
    if (t->req) http_request_free(t->req);
    if (t->res) http_server_response_free(t->res);
    free(t);
}

/* Per-session async-dispatch state init. Sets up the wake pipe +
 * mutex/condvar. Returns 0 on success, -1 on failure. */
static int session_dispatch_init(AetherH2Session* sess) {
    int pipefd[2];
    /* #3046: close-on-exec, so a child the server starts never holds it. */
    if (aether_pipe_cloexec(pipefd) != 0) return -1;

    int fl = fcntl(pipefd[1], F_GETFL, 0);
    if (fl >= 0) fcntl(pipefd[1], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(pipefd[0], F_GETFL, 0);
    if (fl >= 0) fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);

    sess->wake_fd_read  = pipefd[0];
    sess->wake_fd_write = pipefd[1];
    pthread_mutex_init(&sess->ready_mu, NULL);
    pthread_cond_init (&sess->ready_cv, NULL);
    sess->dispatch_enabled = 1;
    return 0;
}

static void session_dispatch_destroy(AetherH2Session* sess) {
    if (!sess->dispatch_enabled) return;
    /* Drain anything still in our ready queue (e.g. workers
     * completed mid-tear-down). The owning request/response are
     * freed via task_free. */
    H2DispatchTask* t = sess->ready_head;
    while (t) { H2DispatchTask* n = t->next; task_free(t); t = n; }
    sess->ready_head = sess->ready_tail = NULL;

    if (sess->wake_fd_read  >= 0) close(sess->wake_fd_read);
    if (sess->wake_fd_write >= 0) close(sess->wake_fd_write);
    pthread_mutex_destroy(&sess->ready_mu);
    pthread_cond_destroy (&sess->ready_cv);
    sess->dispatch_enabled = 0;
}
#endif  /* AETHER_H2_HAS_POOL */

/* dispatch_stream — connection-thread entry point invoked when
 * nghttp2 reports END_STREAM on the request side. We validate
 * pseudo-headers (must RST on the connection thread because nghttp2
 * is single-threaded), allocate the (req, res) pair, then either:
 *
 *   - run the handler synchronously on the connection thread, OR
 *   - hand the task off to a worker pool when concurrent dispatch
 *     is enabled.
 *
 * Either way, submit_response_for_stream runs on the connection
 * thread once the handler has populated `res`. */
static void submit_response_for_stream(AetherH2Session* s,
                                       AetherH2Stream* str,
                                       HttpRequest* req,
                                       HttpServerResponse* res);

static void dispatch_stream(AetherH2Session* s, AetherH2Stream* str) {
    if (str->response_submitted) return;

    /* RFC 7540 §8.1.2.3 — request pseudo-headers :method and :path
     * are mandatory; :scheme is mandatory for non-CONNECT requests
     * (we don't accept CONNECT). A stream missing any of these is
     * malformed and must be rejected with PROTOCOL_ERROR rather
     * than handed to the route table.
     *
     * :authority is recommended but optional (the Host header
     * carries it for HTTP/1.1 compat); we allow it absent because
     * curl --http2-prior-knowledge over plain TCP sometimes omits
     * :authority in favour of the Host header. */
    if (!str->method || !str->path || !str->scheme || str->path[0] == '\0') {
        nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                  str->stream_id, NGHTTP2_PROTOCOL_ERROR);
        str->response_submitted = 1;
        return;
    }

    size_t held = str->body_len;
    HttpRequest* req = request_from_stream(str);
    /* The body went to the request: the connection no longer holds it. */
    if (req && !str->body) s->body_held -= held <= s->body_held ? held : s->body_held;
    if (!req) {
        nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                  str->stream_id, NGHTTP2_INTERNAL_ERROR);
        str->response_submitted = 1;
        return;
    }
    /* The connection's peer, local address and TLS state, as an HTTP/1.1
     * request carries them (#3175). conn_userdata is the HttpConn this
     * session runs on, and this is its thread. */
    http_request_copy_conn_meta(req, (struct HttpConn*)s->conn_userdata);

    HttpServerResponse* res = http_response_create();
    if (!res) {
        http_request_free(req);
        nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                  str->stream_id, NGHTTP2_INTERNAL_ERROR);
        str->response_submitted = 1;
        return;
    }

#ifdef AETHER_H2_HAS_POOL
    /* Concurrent dispatch: run the handler on the shared std.worker pool.
     * h2_task_run pushes the completed task to this session's ready queue and
     * wakes the connection thread (drain_ready submits the response). in_flight
     * is bumped BEFORE submit so session_free can spin on it without racing. */
    if (s->dispatch_enabled) {
        H2DispatchTask* task = calloc(1, sizeof(*task));
        if (!task) {
            http_request_free(req);
            http_server_response_free(res);
            nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                      str->stream_id, NGHTTP2_INTERNAL_ERROR);
            str->response_submitted = 1;
            return;
        }
        task->session = s;
        task->stream  = str;
        task->req     = req;
        task->res     = res;

        pthread_mutex_lock(&s->ready_mu);
        s->in_flight++;
        pthread_mutex_unlock(&s->ready_mu);

        AetherWorkerClosure w = { (void (*)(void))h2_task_run, task };
        if (aether_worker_submit(w)) {
            return;
        }
        /* Pool unavailable (threadless): reclaim in_flight and the task
         * shell, then run sequentially below (the shell must not free req/res
         * the sequential path still owns). */
        pthread_mutex_lock(&s->ready_mu);
        s->in_flight--;
        pthread_mutex_unlock(&s->ready_mu);
        task->req = NULL;
        task->res = NULL;
        task_free(task);
    }
#endif

    /* Sequential path — run handler on the connection thread, then
     * submit the response immediately. Matches the original v1
     * behaviour for builds that don't enable concurrent dispatch
     * (and on Windows where the pool path compiles out). */
    http_server_dispatch_for_h2(s->server, req, res);
    submit_response_for_stream(s, str, req, res);
}

/* submit_response_for_stream — connection-thread side of dispatch.
 * Snapshots `res` into the stream's response buffers, builds the
 * nghttp2_nv array, and calls nghttp2_submit_response. Frees req
 * and res before returning. Must run on the connection thread (the
 * only thread that calls into nghttp2_session). */
static void submit_response_for_stream(AetherH2Session* s,
                                       AetherH2Stream* str,
                                       HttpRequest* req,
                                       HttpServerResponse* res) {
    if (str->response_submitted) {
        if (req) http_request_free(req);
        if (res) http_server_response_free(res);
        return;
    }

    /* Snapshot response into the stream so the data-read callback
     * can stream it out independently of `res`'s lifetime. */
    int status = res->status_code;
    if (status < 100 || status > 999) status = 200;
    str->response_status = status;

    if (res->body && res->body_length > 0) {
        str->response_body = malloc(res->body_length);
        if (str->response_body) {
            memcpy(str->response_body, res->body, res->body_length);
            str->response_body_len = res->body_length;
        }
    }

    /* Status pseudo-header first, then user headers. nghttp2 needs
     * the status as a string; we use a small stack buffer. */
    char status_str[8];
    snprintf(status_str, sizeof(status_str), "%d", str->response_status);

    /* Every field the handler set, plus :status. There is no cap here: the
     * response's arrays grow as fields are set (d5b4cc3ad), and a clamp at
     * 256 dropped every field past it without a word. calloc refuses a
     * count whose size overflows. */
    size_t hdr_count = res->header_count > 0 ? (size_t)res->header_count : 0;

    str->response_headers = calloc(hdr_count + 1, sizeof(nghttp2_nv));
    if (!str->response_headers) {
        http_request_free(req);
        http_server_response_free(res);
        nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                  str->stream_id, NGHTTP2_INTERNAL_ERROR);
        return;
    }

    /* :status pseudo-header. nghttp2 takes name/value as malloc'd
     * buffers we own; sizes are byte-count, NOT including a NUL.
     *
     * NGHTTP2_NV_FLAG_NONE leaves HPACK encoding fully to nghttp2: it
     * automatically applies Huffman encoding to each name/value
     * whenever it produces a shorter representation than the literal
     * (RFC 7541 §5.2). Setting NGHTTP2_NV_FLAG_NO_HUFFMAN here would
     * disable that compression — we don't, so response headers are
     * Huffman-encoded by default. */
    str->response_headers[0].name      = (uint8_t*)dup_str(":status");
    str->response_headers[0].namelen   = 7;
    str->response_headers[0].value     = (uint8_t*)dup_str(status_str);
    str->response_headers[0].valuelen  = strlen(status_str);
    str->response_headers[0].flags     = NGHTTP2_NV_FLAG_NONE;

    size_t real_count = 1;
    for (size_t i = 0; i < hdr_count; i++) {
        const char* hname  = res->header_keys   ? res->header_keys[i]   : NULL;
        const char* hvalue = res->header_values ? res->header_values[i] : NULL;
        if (!hname) continue;
        /* h2 disallows Connection / Transfer-Encoding / Upgrade /
         * Keep-Alive / Proxy-Connection (RFC 7540 §8.1.2.2). Filter
         * them so handlers that emit Connection: close on HTTP/1.1
         * don't accidentally trip the peer. */
        if (strcasecmp(hname, "Connection") == 0 ||
            strcasecmp(hname, "Transfer-Encoding") == 0 ||
            strcasecmp(hname, "Upgrade") == 0 ||
            strcasecmp(hname, "Keep-Alive") == 0 ||
            strcasecmp(hname, "Proxy-Connection") == 0) {
            continue;
        }
        nghttp2_nv* nv = &str->response_headers[real_count];
        nv->name     = (uint8_t*)dup_str(hname);
        nv->namelen  = strlen(hname);
        nv->value    = (uint8_t*)dup_str(hvalue ? hvalue : "");
        nv->valuelen = hvalue ? strlen(hvalue) : 0;
        nv->flags    = NGHTTP2_NV_FLAG_NONE;
        real_count++;
    }
    str->response_header_count = (size_t)real_count;

    /* HEAD responses MUST NOT carry a body (RFC 7230 §4.3.2 for
     * HTTP/1.1, RFC 7540 §8.1 for HTTP/2 — the request method
     * applies the same constraint to either protocol). nghttp2
     * interprets a NULL data_provider as "headers only" and
     * automatically sets END_STREAM on the HEADERS frame, which is
     * the correct wire shape: peers (and curl) reject DATA frames
     * after a HEAD's HEADERS+END_STREAM with PROTOCOL_ERROR. */
    nghttp2_data_provider  data_prd_storage;
    nghttp2_data_provider* data_prd = NULL;
    if (!str->method || strcmp(str->method, "HEAD") != 0) {
        data_prd_storage.source.ptr = NULL;
        data_prd_storage.read_callback = ng_response_data_read_callback;
        data_prd = &data_prd_storage;
    }

    int rv = nghttp2_submit_response(s->ng, str->stream_id,
                                     str->response_headers,
                                     str->response_header_count,
                                     data_prd);
    str->response_submitted = 1;
    /* The connection's idle clock starts over: a handler that took longer
     * than the idle timeout has its response go out in full (#3738). */
    s->last_progress_ms = aether_h2_now_ms();

    http_request_free(req);
    http_server_response_free(res);

    if (rv != 0) {
        nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                  str->stream_id, NGHTTP2_INTERNAL_ERROR);
    }
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

AetherH2Session* aether_h2_session_new(HttpServer* server,
                                       void* conn_userdata) {
    if (!server) return NULL;

    AetherH2Session* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->server = server;
    s->conn_userdata = conn_userdata;

    nghttp2_session_callbacks* cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        free(s);
        return NULL;
    }
    nghttp2_session_callbacks_set_send_callback(cbs, ng_send_callback);
    nghttp2_session_callbacks_set_on_header_callback(cbs, ng_on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, ng_on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, ng_on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, ng_on_stream_close);

    /* nghttp2's protections, set explicitly so the server's limits are
     * written down in one place rather than left to a library version's
     * defaults (#3750). The values are nghttp2's own defaults where it has
     * them. Each option exists from the release named; built against an
     * older nghttp2 the session goes without it (the build warns, see the
     * Makefile). */
    nghttp2_option* opt = NULL;
    if (nghttp2_option_new(&opt) != 0) opt = NULL;
    if (opt) {
#if NGHTTP2_VERSION_NUM >= 0x012800   /* 1.40: SETTINGS and PING ack floods */
        nghttp2_option_set_max_outbound_ack(opt, 1000);
#endif
#if NGHTTP2_VERSION_NUM >= 0x012800   /* 1.40 */
        nghttp2_option_set_max_settings(opt, 32);
#endif
#if NGHTTP2_VERSION_NUM >= 0x013c00   /* 1.60: rapid reset, CVE-2023-44487 */
        nghttp2_option_set_stream_reset_rate_limit(opt, 1000, 33);
#endif
#if NGHTTP2_VERSION_NUM >= 0x013d00   /* 1.61: CONTINUATION flood, CVE-2024-28182 */
        nghttp2_option_set_max_continuations(opt, 8);
#endif
    }
    int rv = nghttp2_session_server_new2(&s->ng, cbs, s, opt);
    if (opt) nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(cbs);
    if (rv != 0) {
        free(s);
        return NULL;
    }

#ifdef AETHER_H2_HAS_POOL
    /* Concurrent dispatch runs handlers on the shared std.worker pool and
     * needs this session's per-connection wake-pipe + ready-queue plumbing.
     * session_dispatch_init leaves dispatch_enabled at 0 on setup failure, so
     * this connection just falls back to sequential dispatch. */
    s->wake_fd_read = -1;
    s->wake_fd_write = -1;
    if (server->h2_dispatch_workers > 0) {
        aether_worker_pool_configure(server->h2_dispatch_workers);
        session_dispatch_init(s);
    }
#endif

    return s;
}

void aether_h2_session_free(AetherH2Session* sess) {
    if (!sess) return;

#ifdef AETHER_H2_HAS_POOL
    /* Wait for any in-flight worker tasks to finish before tearing
     * the session down — workers may still be running handlers
     * that touch this session's state. The connection driver loop
     * is also responsible for calling drain_ready before we get
     * here, so most ready tasks should already be submitted; this
     * is the safety net for tasks whose workers were still running
     * when the connection closed. */
    if (sess->dispatch_enabled) {
        pthread_mutex_lock(&sess->ready_mu);
        while (sess->in_flight > 0 || sess->ready_head != NULL) {
            /* Pull any ready tasks the connection driver missed,
             * then keep waiting on workers still mid-handler. */
            if (sess->ready_head) {
                pthread_mutex_unlock(&sess->ready_mu);
                aether_h2_session_drain_ready(sess);
                pthread_mutex_lock(&sess->ready_mu);
                continue;
            }
            pthread_cond_wait(&sess->ready_cv, &sess->ready_mu);
        }
        pthread_mutex_unlock(&sess->ready_mu);
        session_dispatch_destroy(sess);
    }
#endif

    if (sess->ng) nghttp2_session_del(sess->ng);
    AetherH2Stream* str = sess->streams;
    while (str) {
        AetherH2Stream* nxt = str->next;
        stream_free(str);
        str = nxt;
    }
    free(sess->out_buf);
    free(sess);
}

int aether_h2_session_send_initial_settings(AetherH2Session* sess) {
    if (!sess || !sess->ng) return -1;
    nghttp2_settings_entry iv[3];
    int n = 0;
    iv[n].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    iv[n++].value     = 1 << 20;  /* 1 MiB initial flow-control window */

    /* Always sent (#3738). It was sent only when configured, and
     * nghttp2's local default is unlimited, so a client could open as
     * many streams as it liked on one connection, each holding a request
     * and its body. 100 is the floor RFC 9113 5.1.2 recommends. */
    iv[n].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv[n++].value     = sess->server->limits.h2_max_concurrent_streams > 0
                      ? (uint32_t)sess->server->limits.h2_max_concurrent_streams
                      : (uint32_t)HTTP_H2_DEFAULT_MAX_CONCURRENT_STREAMS;

    /* The header list the server keeps (#3738), so a client knows the
     * bound before it sends a list past it (answered 431). */
    iv[n].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
    iv[n++].value     = sess->server->limits.h2_max_header_list > 0
                      ? (uint32_t)sess->server->limits.h2_max_header_list
                      : (uint32_t)HTTP_H2_DEFAULT_MAX_HEADER_LIST;

    return nghttp2_submit_settings(sess->ng, NGHTTP2_FLAG_NONE, iv, (size_t)n);
}

int aether_h2_session_feed(AetherH2Session* sess,
                           const uint8_t* data, size_t len) {
    if (!sess || !sess->ng) return -1;
    ssize_t consumed = nghttp2_session_mem_recv(sess->ng, data, len);
    if (consumed < 0) {
        sess->fatal = 1;
        return -1;
    }
    /* nghttp2_session_mem_recv consumes the whole input buffer or
     * returns the count actually parsed; in non-error paths it
     * always equals len. */
    return (int)consumed;
}

int aether_h2_session_drain(AetherH2Session* sess,
                            AetherH2WriteFn write_fn) {
    if (!sess || !sess->ng || !write_fn) return -1;

    /* Pump nghttp2's send loop — this calls our ng_send_callback
     * to fill out_buf with serialised frames. */
    int rv = nghttp2_session_send(sess->ng);
    if (rv != 0) {
        sess->fatal = 1;
        return -1;
    }

    /* Flush whatever's queued. The wire callback may take a partial
     * write; we leave the rest for the next drain. */
    if (sess->out_buf_len > 0) {
        int written = write_fn(sess->conn_userdata,
                               sess->out_buf, sess->out_buf_len);
        if (written < 0) return -1;
        if (written > 0) sess->last_progress_ms = aether_h2_now_ms();
        if ((size_t)written < sess->out_buf_len) {
            memmove(sess->out_buf, sess->out_buf + written,
                    sess->out_buf_len - written);
            sess->out_buf_len -= (size_t)written;
        } else {
            sess->out_buf_len = 0;
        }
    }

    if (sess->fatal) return -1;
    if (aether_h2_session_want_close(sess)) return 1;
    return 0;
}

int aether_h2_session_wake_fd(AetherH2Session* sess) {
#ifdef AETHER_H2_HAS_POOL
    if (sess && sess->dispatch_enabled) return sess->wake_fd_read;
#else
    (void)sess;
#endif
    return -1;
}

int aether_h2_session_drain_ready(AetherH2Session* sess) {
#ifdef AETHER_H2_HAS_POOL
    if (!sess || !sess->dispatch_enabled) return 0;

    /* Drain whatever wake bytes are sitting in the pipe. We read
     * eagerly into a small buffer because each completed task wrote
     * exactly one byte; the pipe was set non-blocking so a quiet
     * pipe returns EAGAIN immediately. The actual queue is the
     * authoritative source — the wake byte just unblocks poll(). */
    char drain[64];
    while (1) {
        ssize_t n = read(sess->wake_fd_read, drain, sizeof(drain));
        if (n <= 0) break;
    }

    int submitted = 0;
    while (1) {
        pthread_mutex_lock(&sess->ready_mu);
        H2DispatchTask* task = sess->ready_head;
        if (task) {
            sess->ready_head = task->next;
            if (!sess->ready_head) sess->ready_tail = NULL;
        }
        pthread_mutex_unlock(&sess->ready_mu);
        if (!task) break;

        submit_response_for_stream(sess, task->stream, task->req, task->res);
        /* req + res were consumed by submit_response_for_stream. */
        free(task);

        /* Decrement in_flight + signal anyone waiting (session_free
         * blocks on in_flight reaching 0 during teardown). */
        pthread_mutex_lock(&sess->ready_mu);
        sess->in_flight--;
        if (sess->in_flight == 0) pthread_cond_broadcast(&sess->ready_cv);
        pthread_mutex_unlock(&sess->ready_mu);

        submitted++;
    }
    return submitted;
#else
    (void)sess;
    return 0;
#endif
}

int aether_h2_session_want_close(AetherH2Session* sess) {
    if (!sess || !sess->ng) return 1;
    if (sess->fatal) return 1;
    /* When concurrent dispatch is on, we must NOT report want_close
     * while tasks are still in flight. Workers may have responses
     * about to land; closing the session early would discard them. */
#ifdef AETHER_H2_HAS_POOL
    if (sess->dispatch_enabled) {
        pthread_mutex_lock(&sess->ready_mu);
        int still_busy = (sess->in_flight > 0) || (sess->ready_head != NULL);
        pthread_mutex_unlock(&sess->ready_mu);
        if (still_busy) return 0;
    }
#endif
    int want_read  = nghttp2_session_want_read(sess->ng);
    int want_write = nghttp2_session_want_write(sess->ng);
    return (!want_read && !want_write) ? 1 : 0;
}

int64_t aether_h2_session_last_active(AetherH2Session* sess) {
    return sess ? sess->last_active_ms : 0;
}

int64_t aether_h2_session_last_progress(AetherH2Session* sess) {
    return sess ? sess->last_progress_ms : 0;
}

void aether_h2_session_refuse_incomplete(AetherH2Session* sess) {
    if (!sess || !sess->ng) return;
    for (AetherH2Stream* str = sess->streams; str; str = str->next) {
        if (str->request_done || str->response_submitted) continue;
        sess->body_held -= str->body_len <= sess->body_held ? str->body_len : sess->body_held;
        free(str->body);
        str->body = NULL;
        str->body_len = str->body_cap = 0;
        str->body_refused = 1;
        str->response_submitted = 1;
        nghttp2_submit_rst_stream(sess->ng, NGHTTP2_FLAG_NONE, str->stream_id,
                                  NGHTTP2_REFUSED_STREAM);
    }
}

int aether_h2_session_busy(AetherH2Session* sess) {
    if (!sess) return 0;
#ifdef AETHER_H2_HAS_POOL
    if (sess->dispatch_enabled) {
        pthread_mutex_lock(&sess->ready_mu);
        int busy = sess->in_flight > 0 || sess->ready_head != NULL;
        pthread_mutex_unlock(&sess->ready_mu);
        if (busy) return 2;
    }
#endif
    return sess->streams ? 1 : 0;
}

/* Each stream's request must arrive whole within the server's head and
 * body budgets of its first HEADERS (#3738), the body's extended by a second
 * for each body_min_rate bytes it sent: the HTTP/1.1 limits (#3737), by the
 * clock, since one connection carries many streams and no read waits on any
 * one of them. A stream that sent a DATA frame now and then, each inside
 * the idle timeout, held its request open, and its connection and worker
 * with it, for as long as it liked. The credit a stream's bytes earn is held
 * to the rate window (limits.body_rate_window_ms): kept whole, a burst then a
 * trickle held the stream for hours. A late stream is reset with CANCEL and
 * what it sent is freed. A stream being handled, or answered, is past it. */
int64_t aether_h2_session_expire(AetherH2Session* sess, int64_t now_ms) {
    if (!sess || !sess->ng || !sess->server) return 0;
    const HttpServerLimits* lim = &sess->server->limits;
    int64_t head = lim->head_timeout_ms > 0 ? lim->head_timeout_ms : 0;
    int64_t body = lim->body_timeout_ms > 0 ? lim->body_timeout_ms : 0;
    if (head == 0 && body == 0) return 0;
    int64_t next = 0;
    for (AetherH2Stream* str = sess->streams; str; str = str->next) {
        if (str->request_done || str->response_submitted) continue;
        if (!str->due_ms) str->due_ms = str->opened_ms + head + body;
        if (lim->body_min_rate > 0 && str->body_len > str->body_credited) {
            int64_t credit = (int64_t)str->body_len * 1000 / lim->body_min_rate -
                             (int64_t)str->body_credited * 1000 / lim->body_min_rate;
            str->due_ms = now_ms + http_body_budget_credit(str->due_ms - now_ms, credit,
                                                           lim->body_rate_window_ms);
            str->body_credited = str->body_len;
        }
        int64_t due = str->due_ms;
        if (now_ms >= due) {
            sess->body_held -= str->body_len <= sess->body_held ? str->body_len : sess->body_held;
            free(str->body);
            str->body = NULL;
            str->body_len = str->body_cap = 0;
            str->body_refused = 1;
            str->response_submitted = 1;
            nghttp2_submit_rst_stream(sess->ng, NGHTTP2_FLAG_NONE, str->stream_id,
                                      NGHTTP2_CANCEL);
            continue;
        }
        if (next == 0 || due < next) next = due;
    }
    return next;
}

void aether_h2_session_initiate_goaway(AetherH2Session* sess) {
    if (!sess || !sess->ng) return;
    /* A graceful GOAWAY (RFC 9113 6.8): no new streams past the last one
     * processed, and the streams already open complete normally. Once it
     * is sent and none is left, nghttp2_session_want_read and _want_write
     * turn false, which want_close keys on. It was
     * nghttp2_session_terminate_session, which ends the session as soon as
     * its GOAWAY is out (both turn false at once), so a graceful shutdown
     * dropped the streams it meant to let finish; an HTTP/2 connection that
     * reaches its age (#3738) must let them finish too. */
    nghttp2_submit_goaway(sess->ng, NGHTTP2_FLAG_NONE,
                          nghttp2_session_get_last_proc_stream_id(sess->ng),
                          NGHTTP2_NO_ERROR, NULL, 0);
}

/* ------------------------------------------------------------------
 * h2c (cleartext) upgrade — RFC 7540 §3.2.
 *
 * The HTTP/1.1 parser delivers the request that asked for the
 * upgrade plus the base64url-encoded HTTP2-Settings header value.
 * We:
 *   1. Decode the SETTINGS payload into the session's peer-side
 *      pre-loaded state (nghttp2_session_upgrade2 does this).
 *   2. Synthesise stream 1 from the original HTTP/1.1 request so
 *      the route handler sees a normal h2 stream.
 *   3. Write 101 Switching Protocols on the wire (caller responsible
 *      via the supplied callback before any h2 frames flow).
 *
 * The 101 emission is the caller's job — we just hand back the new
 * session pre-loaded with the synthesised stream. The caller writes
 * the 101 line before the first drain.
 * ------------------------------------------------------------------ */

AetherH2Session* aether_h2_session_from_h2c_upgrade(
    HttpServer* server,
    void* conn_userdata,
    const char* request_method,
    const char* request_path,
    const char* request_headers,
    const char* http2_settings_b64) {
    if (!server || !request_method || !request_path) return NULL;

    AetherH2Session* sess = aether_h2_session_new(server, conn_userdata);
    if (!sess) return NULL;

    /* Decode SETTINGS payload (base64url, RFC 7540 §3.2.1) with
     * std.cryptography's decoder, the only one in the tree. RFC 7540 says
     * the client leaves off the padding; trailing `=` is accepted anyway. */
    size_t settings_len = 0;
    uint8_t* settings_buf = NULL;
    if (http2_settings_b64) {
        settings_buf = cryptography_base64_decode_buf(http2_settings_b64,
                                                      strlen(http2_settings_b64),
                                                      1, &settings_len);
        if (!settings_buf) {
            aether_h2_session_free(sess);
            return NULL;
        }
    }

    /* Hand the peer SETTINGS to nghttp2 and synthesise stream 1. */
    int rv = nghttp2_session_upgrade2(sess->ng,
                                      settings_buf, settings_len,
                                      strcmp(request_method, "HEAD") == 0 ? 1 : 0,
                                      NULL);
    free(settings_buf);
    if (rv != 0) {
        aether_h2_session_free(sess);
        return NULL;
    }

    /* Pre-populate stream 1 with the original HTTP/1.1 request so
     * the dispatcher sees the same shape an h2 client would have
     * sent. */
    AetherH2Stream* str = stream_get_or_create(sess, 1);
    if (!str) {
        aether_h2_session_free(sess);
        return NULL;
    }
    str->method = dup_str(request_method);
    str->path   = dup_str(request_path);
    /* h2c upgrades come in over a plain (cleartext) socket, so the
     * synthesised stream-1 carries `:scheme: http`. The h2-over-TLS
     * path doesn't go through this entry point — peers send their
     * own :scheme via HEADERS. */
    str->scheme = dup_str("http");
    if (request_headers) {
        size_t hlen = strlen(request_headers);
        buf_append(&str->header_block, &str->header_block_len,
                   &str->header_block_cap, request_headers, hlen);
    }
    str->request_done = 1;
    dispatch_stream(sess, str);

    /* Send the SETTINGS frame the server must emit first. */
    aether_h2_session_send_initial_settings(sess);
    return sess;
}

#else  /* !AETHER_HAS_NGHTTP2 — no-op stubs */

AetherH2Session* aether_h2_session_new(struct HttpServer* server,
                                       void* conn_userdata) {
    (void)server; (void)conn_userdata; return NULL;
}
void aether_h2_session_free(AetherH2Session* sess) { (void)sess; }
int aether_h2_session_send_initial_settings(AetherH2Session* sess) {
    (void)sess; return -1;
}
int aether_h2_session_feed(AetherH2Session* sess,
                           const uint8_t* data, size_t len) {
    (void)sess; (void)data; (void)len; return -1;
}
int aether_h2_session_drain(AetherH2Session* sess,
                            AetherH2WriteFn write_fn) {
    (void)sess; (void)write_fn; return -1;
}
int aether_h2_session_wake_fd(AetherH2Session* sess) {
    (void)sess; return -1;
}
int aether_h2_session_drain_ready(AetherH2Session* sess) {
    (void)sess; return 0;
}
int aether_h2_session_want_close(AetherH2Session* sess) {
    (void)sess; return 1;
}
void aether_h2_session_initiate_goaway(AetherH2Session* sess) {
    (void)sess;
}
int aether_h2_session_busy(AetherH2Session* sess) {
    (void)sess; return 0;
}
int64_t aether_h2_session_last_active(AetherH2Session* sess) {
    (void)sess; return 0;
}
int64_t aether_h2_session_last_progress(AetherH2Session* sess) {
    (void)sess; return 0;
}
void aether_h2_session_refuse_incomplete(AetherH2Session* sess) {
    (void)sess;
}
int64_t aether_h2_session_expire(AetherH2Session* sess, int64_t now_ms) {
    (void)sess; (void)now_ms; return 0;
}
AetherH2Session* aether_h2_session_from_h2c_upgrade(
    struct HttpServer* server,
    void* conn_userdata,
    const char* request_method,
    const char* request_path,
    const char* request_headers,
    const char* http2_settings_b64) {
    (void)server; (void)conn_userdata; (void)request_method;
    (void)request_path; (void)request_headers; (void)http2_settings_b64;
    return NULL;
}

#endif  /* AETHER_HAS_NGHTTP2 */
