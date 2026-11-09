/* aether_http_evloop.c — the proxy driver that does not wait on one connection.
 *
 * See aether_http_evloop.h for why this exists. The short version: a worker
 * that owns a single connection has nothing to run while that connection
 * waits, so it sleeps, twice per proxied request. A thread holding many
 * connections always has something else to do.
 *
 * One connection is one state machine. Every state ends either by making
 * progress or by naming the descriptor and the readiness it needs next; the
 * driver arms that and moves on to another connection. Nothing here blocks.
 */

#include "aether_http_evloop.h"
#include "aether_http_server.h"

/* The driver needs three things: threads to run on, a poller to wait in, and
 * a pipe to hand it a descriptor without a lock. Windows has no descriptor a
 * pipe and a socket can share a poller through, and wasi has no pipe at all,
 * so on both the server keeps the per-connection path it already had and
 * nothing here is compiled. Same shape as the parking lot's fallback. */
#if !defined(_WIN32) && !defined(__wasi__) && !defined(__EMSCRIPTEN__)
#define AETHER_EVLOOP_SUPPORTED 1
#endif

#if defined(AETHER_EVLOOP_SUPPORTED) && AETHER_HAS_THREADS

#include "aether_http.h"
#include "aether_http_internal.h"
#include "aether_http_pool.h"
#include "../http/proxy/aether_proxy_internal.h"
#include "../../runtime/scheduler/aether_io_poller.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/utils/aether_thread.h"
#include "../../runtime/utils/aether_spawn.h"
#include <sys/uio.h>

/* Linux names this on the call; the BSDs and macOS have SO_NOSIGPIPE on the
 * socket instead and no flag, where zero is right and the server's ignored
 * disposition is what covers it. */
#if defined(MSG_NOSIGNAL)
#define AE_SEND_NOSIGNAL MSG_NOSIGNAL
#else
#define AE_SEND_NOSIGNAL 0
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifdef AETHER_HAS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#endif

typedef enum {
    EV_TLS_HANDSHAKE,    /* completing a TLS handshake with the client */
    EV_READ_REQUEST,     /* reading a request from the client */
    EV_UPSTREAM_LOOKUP,  /* waiting on the loop's resolver for the upstream's
                          * name (#3267) */
    EV_UPSTREAM_DIAL,    /* a connect this driver started is still in flight */
    EV_UPSTREAM_SEND,    /* writing the request to the upstream */
    EV_UPSTREAM_RECV,    /* reading the response from the upstream */
    EV_BACKOFF,          /* waiting out a retry's backoff, on a timer (#3232) */
    EV_CLIENT_SEND,      /* writing the response to the client */
    EV_CLOSING
} EvState;

typedef struct EvConn {
    int      client_fd;
    EvState  state;

    char*    in;            /* request bytes from the client */
    size_t   in_len, in_cap;
    size_t   in_scanned;    /* how much of `in` has been searched for the
                             * end of the headers; a read that arrives in
                             * pieces would otherwise rescan from the start
                             * every time */
    int      client_in_poller, up_in_poller;
    /* On a backend with no edge mode, the interest each descriptor holds while
     * it is in the poller, set by ev_level_settle (#3258). */
    uint32_t client_events, up_events;
    /* What the client's TLS session last waited for: 1 to read, 2 to write,
     * which a level-triggered backend has to know to wait on the right
     * readiness (#3258). */
    int      tls_want;
#ifdef AETHER_HAS_OPENSSL
    /* The client's TLS session, when this connection has one. A TLS
     * connection used to be refused by this driver and given a thread of its
     * own instead, which is the cost the driver exists to remove and the
     * shape most proxied traffic actually arrives in. */
    SSL*     ssl;
#endif
    size_t   in_hdr_end;    /* offset just past the header terminator, once
                             * found. Kept because a request whose body is
                             * still arriving is asked again, and resuming the
                             * search would then look past the terminator and
                             * never find it. 0 means not found yet */
    /* The head's verdict, made once when its end is found (#3269): -1 the
     * worker path's to answer, else the body length it declares. Checked on
     * every read of the body before, the head scanned again each time. */
    long     in_declared;

    /* Bytes of `in` belonging to the request being served. A client may send
     * the next request in the same segment; what sits past this offset is
     * that request, and dropping it strands a request the client is waiting
     * on. */
    size_t   in_total;

    char*    out;           /* response bytes owed to the client */
    size_t   out_len, out_sent, out_cap;
    /* A pass-through answers from the bytes the upstream sent: the head is
     * built into `out` and the body is sent straight from the receive buffer,
     * so it is never copied. NULL when the response was copied the ordinary
     * way and `out` already holds all of it. */
    const char* out_body;
    size_t      out_body_len, out_body_sent;

    HttpUpstreamConn up;
    HttpExchange     x;
    char*            head;       /* serialised upstream request */
    size_t           head_len, head_cap;

    int64_t  idle_due;           /* when a quiet client is closed, 0 = not
                                  * waiting on it (ev_arm_idle, #3269) */
    int64_t  deadline_ms;        /* when the upstream call gives up, 0 = none;
                                  * a `long` before, 32 bits on Windows, where
                                  * the clock went negative after 24.8 days of
                                  * uptime and every deadline read as none
                                  * (#3096) */
    int      up_writable_watched; /* write interest on the upstream, which is
                                   * only wanted while a write is blocked */
    int      up_watched;         /* registered with the poller at all */
    /* A connection taken from the pool that turned out to be closed has been
     * dialled again once. One retry only: a fresh connection that answers
     * nothing is the upstream's answer, not a stale descriptor. */
    int      up_stale_retried;   /* per request (#3261) */
    /* The next dial is that retry: a fresh connection, not another from the
     * pool, which may hold more that died the same way, as the worker path's
     * client does (#3260). One-shot; ev_dial clears it. */
    int      dial_fresh;
    /* The declared body this request holds against the loop's total
     * (ev_request_complete), given back when the request ends (#3263). */
    size_t   body_reserved;
    /* Waiting on the loop's resolver (EV_UPSTREAM_LOOKUP, #3267): on this
     * driver's list of such connections, which ev_resume_lookups dials
     * again when the name it waits on (lookup_name, http_resolver_name) is
     * answered. */
    int      lookup_waiting;
    unsigned lookup_name;
    struct EvConn* lookup_prev;
    struct EvConn* lookup_next;
    /* This attempt's upstream deadline was armed when it began waiting on
     * the resolver, and the dial after the wait keeps it: the lookup counts
     * in the upstream timeout, once (#3283). */
    int      lookup_deadline;

    /* The response accumulator, kept between requests. It starts at 16 KiB,
     * and allocating and freeing one per request had the kernel handing back
     * fresh pages and zeroing them: page clearing was the single largest
     * entry in the profile. A connection owns its buffer for its life. */
    char*    rxbuf;
    size_t   rxcap;

    /* Where the outbound request's headers are built. One request's worth of
     * small allocations, released by resetting an offset rather than by a
     * free per header: building them was the largest identifiable source of
     * page faults on this path. */
    HttpArena arena;
    int       arena_ready;
    int      heap_pos;           /* where this sits in its driver's heap, -1 out */

    HttpRequest*        req;     /* the parsed client request */
    HttpServerResponse* res;     /* what the proxy fills in for the client */
    AetherProxyExchange px;      /* the proxy's own resumable state */

    /* The connection's addresses and TLS state, resolved once, for each
     * request built on it (#3169, #3214). */
    HttpConnMeta meta;
    int          meta_ready;

    /* What runs around the proxy on the current request (#3214).
     * `before_proxy`: the server's middleware ahead of the proxy runs on it,
     * so the request object is this request's own (ev_begin_upstream).
     * `hooked`: the server's request hooks observe its response, from the
     * clock reading taken when its first byte was seen. */
    int      before_proxy;
    int      hooked;
    int64_t  t_start_us;
    /* Responses written on this connection, the worker path's
     * requests_served, which its keep-alive policy counts. */
    int      requests_served;
    /* The response being written said Connection: close (a middleware's
     * answer, settled as the worker path settles it): the connection ends
     * once it is out. */
    int      close_after;

    struct EvConn* next;         /* free list / owner list */
} EvConn;

/* One driver: a thread, its poller, and the connections it owns.
 *
 * A connection belongs to exactly one driver for its whole life, and so does
 * its upstream. That is the point of the design rather than a detail of it:
 * the cost being removed here is work crossing a thread boundary, so nothing
 * in a request may cross one. */
typedef struct {
    HttpEvLoop*    loop;
    int            index;
    pthread_t      thread;
    AetherIoPoller poller;
    int            edge_triggered;   /* backend honours AETHER_IO_EDGE */
    EvConn**       by_fd;        /* descriptor -> the connection using it */
    int            by_fd_cap;
    int            wake_r;       /* a submitted descriptor arrives here */
    int            wake_w;
    /* Running: read by the accept path and the resolver's threads, which
     * write to wake_w only while it is (ev_names_answered). */
    atomic_int     started;
    /* Connections waiting on the resolver (EV_UPSTREAM_LOOKUP), and whether
     * it has answered since this driver last looked (#3267). The names it
     * answered meanwhile, by http_resolver_name, for this driver to wake
     * only the connections waiting on them (#3283); `answered_all` when more
     * came than the list holds. Written by the resolver's threads, read by
     * this driver, under answered_lock. */
    EvConn*        lookups;
    atomic_int     answered;
    pthread_mutex_t answered_lock;
    unsigned       answered_names[16];
    int            answered_count;
    int            answered_all;

    /* Connections with a deadline, earliest first. A proxy has to give up on
     * an upstream that never answers, and the loop needs to know when the
     * next one is due without walking everything it owns. */
    EvConn**       timers;
    int            timer_count, timer_cap;

    /* Returns from the poller with events, for http_evloop_wakeups. Written
     * by this driver alone; read relaxed by whoever asks (#3258). */
    _Atomic int64_t wakeups;
    int64_t        next_idle_sweep_ms;   /* ev_sweep_idle (#3269) */

    /* What serving a request here takes from the server (whether the loop
     * may, the proxy's options, the middleware ahead of it), kept and
     * recomputed only when the server says it changed: three walks of the
     * chain on every request before (#3269). */
    HttpLoopPlan   plan;
} EvDriver;

struct HttpEvLoop {
    HttpServer* server;
    EvDriver*   drivers;
    int         driver_count;
    atomic_int  active;
    atomic_int  taken;           /* connections taken since the start (#3214) */
    atomic_int  stopping;
    atomic_int  next_driver;     /* round-robin placement of new connections */
    /* Request bodies over the worker's buffer size the drivers hold now, by
     * declared length, against body_slots bodies of the mount's cap: the
     * workers the worker path would run (#3263). */
    atomic_llong body_held;
    int          body_slots;
    /* The upstreams' names, looked up off the drivers' threads (#3267). */
    HttpResolver* resolver;
};

/* What the resolver writes down a driver's pipe, where the accept path writes
 * a descriptor and a stop writes -1 (ev_names_answered). */
#define EV_NOTE_ANSWERED (-2)

static int ev_timer_set(EvDriver* d, EvConn* c, int64_t deadline_ms);

/* Nagle holds a small write back until the peer acknowledges what is already
 * in flight. On a proxied connection the response is one such write, and the
 * peer, having nothing to send, only acknowledges when its delayed-ACK timer
 * says so. The two together add a standalone acknowledgement in each
 * direction that carries no data, and the kernel pays full TCP receive
 * processing for each: measured at 5.94 segments per request against nginx's
 * 4.02, which is the number a proxied request needs.
 *
 * The upstream socket has had this since it was first dialled; the accepted
 * one never did. */
static void ev_set_nodelay(int fd) {
#if defined(TCP_NODELAY)
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const void*)&on, sizeof(on));
#endif
}

static void ev_set_nonblocking(int fd) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

int http_evloop_active(HttpEvLoop* loop) {
    return loop ? atomic_load(&loop->active) : 0;
}

int http_evloop_taken(HttpEvLoop* loop) {
    return loop ? atomic_load(&loop->taken) : 0;
}

int64_t http_evloop_wakeups(HttpEvLoop* loop) {
    int64_t total = 0;
    if (!loop) return 0;
    for (int i = 0; i < loop->driver_count; i++)
        total += atomic_load_explicit(&loop->drivers[i].wakeups, memory_order_relaxed);
    return total;
}

struct HttpResolver* http_evloop_resolver(HttpEvLoop* loop) {
    return loop ? loop->resolver : NULL;
}

/* ---- buffers ---- */

static int ev_in_reserve(EvConn* c, size_t extra) {
    if (c->in_len + extra + 1 <= c->in_cap) return 0;
    size_t want = c->in_cap ? c->in_cap * 2 : 8192;
    while (want < c->in_len + extra + 1) want *= 2;
    char* grown = (char*)realloc(c->in, want);
    if (!grown) return -1;
    c->in = grown;
    c->in_cap = want;
    return 0;
}

/* What the proxy exchange built for an attempt, released once the attempt is
 * over, as the blocking driver releases it after its send: the outbound
 * request (its copy of the body, its content type, any header past the
 * arena) and its URL. The exchange's body pointer points into the outbound
 * request, so not before the upstream answered or failed. Clearing the
 * exchange used to drop both, every proxied request leaking its URL and one
 * with a body the copy of it too (#3242). */
static void ev_release_attempt(EvConn* c) {
    if (c->px.outbound) http_request_free_raw(c->px.outbound);
    free(c->px.upstream_url);
    c->px.outbound = NULL;
    c->px.upstream_url = NULL;
}

/* Everything one request owned, released before the next one starts.
 *
 * A connection serves many requests, so anything allocated per request has to
 * be freed per request: what leaks here does not leak once, it leaks for as
 * long as the client keeps talking. The head and the response accumulator go
 * through the capability allocator, so they come back through it too, with
 * the size they were charged at. */
static void ev_conn_reset_request(EvDriver* d, EvConn* c) {
    /* The request's share of the loop's body total goes back with it, on
     * every way a request ends: answered, handed to the worker path, closed
     * (#3263). */
    if (c->body_reserved) {
        atomic_fetch_sub(&d->loop->body_held, (long long)c->body_reserved);
        c->body_reserved = 0;
    }
    size_t carried = c->in_len > c->in_total ? c->in_len - c->in_total : 0;
    if (carried) memmove(c->in, c->in + c->in_total, carried);
    c->in_len = carried;
    /* A buffer grown for one large body goes back to the first size once
     * that request is done, as the client pool's receive buffers do: kept,
     * it pinned the largest body for the connection's life (#3262). */
    if (c->in_cap > HTTP_RX_KEEP_MAX && carried + 1 <= 8192) {
        char* small = (char*)realloc(c->in, 8192);
        if (small) { c->in = small; c->in_cap = 8192; }
    }
    c->in_total = 0;
    c->in_scanned = 0;
    c->in_hdr_end = 0;

    /* The head buffer stays with the connection and is only grown. Every
     * request on it builds a head of about the same size, so after the first
     * there is nothing to allocate. */
    c->head_len = 0;

    /* Keep the accumulator rather than returning it; the next request on this
     * connection reuses it. Up to HTTP_RX_KEEP_MAX, as a pooled connection
     * keeps one: kept at any size, one large response pinned that much for
     * the connection's life (#3262). */
    if (c->x.buf) {
        if (c->x.cap <= HTTP_RX_KEEP_MAX) {
            if (c->rxbuf) aether_caps_free(c->rxbuf, c->rxcap);
            c->rxbuf = c->x.buf;
            c->rxcap = c->x.cap;
        } else {
            aether_caps_free(c->x.buf, c->x.cap);
        }
    }
    memset(&c->x, 0, sizeof(c->x));

    /* The buffer stays with the connection; a connection serving many
     * requests stops allocating and freeing one per request. Not one grown
     * past HTTP_RX_KEEP_MAX for a large response (#3262). */
    if (c->out_cap > HTTP_RX_KEEP_MAX) {
        free(c->out);
        c->out = NULL;
        c->out_cap = 0;
    }
    c->out_len = c->out_sent = 0;
    c->out_body = NULL;
    c->out_body_len = c->out_body_sent = 0;

    /* The request and the response objects stay with the connection. Each
     * one costs a handful of allocations to build, including two fixed-size
     * arrays of header slots that are identical every time, and a connection
     * serves many requests. They are reset in place when the next request
     * arrives rather than freed and built again. Except a request the
     * middleware ahead of the proxy ran on, which is that request's own and
     * goes with it (ev_begin_upstream, #3214). */
    ev_release_attempt(c);
    memset(&c->px, 0, sizeof(c->px));
    if (c->before_proxy && c->req) {
        http_request_free(c->req);
        c->req = NULL;
    }
    c->before_proxy = 0;
    c->hooked = 0;
    c->t_start_us = 0;
    c->close_after = 0;
    /* The stale-connection retry is each request's: kept from one request to
     * the next, a client connection's later requests that drew a dead pooled
     * upstream were answered 502 instead of being sent again (#3261). */
    c->up_stale_retried = 0;
    c->dial_fresh = 0;
    c->lookup_deadline = 0;
}

/* Remember which connection a descriptor belongs to. The poller reports a
 * bare descriptor, so the driver needs the way back. */
static int ev_track(EvDriver* d, int fd, EvConn* c) {
    if (fd < 0) return -1;
    if (fd >= d->by_fd_cap) {
        int want = d->by_fd_cap ? d->by_fd_cap : 256;
        while (want <= fd) want *= 2;
        EvConn** grown = (EvConn**)realloc(d->by_fd, (size_t)want * sizeof(*grown));
        if (!grown) return -1;
        memset(grown + d->by_fd_cap, 0,
               (size_t)(want - d->by_fd_cap) * sizeof(*grown));
        d->by_fd = grown;
        d->by_fd_cap = want;
    }
    d->by_fd[fd] = c;
    return 0;
}

static void ev_untrack(EvDriver* d, int fd) {
    if (fd >= 0 && fd < d->by_fd_cap) d->by_fd[fd] = NULL;
}

/* Off its driver's list of connections waiting on the resolver, if it is
 * on it (#3267). */
static void ev_lookup_unlink(EvDriver* d, EvConn* c) {
    if (!c->lookup_waiting) return;
    if (c->lookup_prev) c->lookup_prev->lookup_next = c->lookup_next;
    else                d->lookups = c->lookup_next;
    if (c->lookup_next) c->lookup_next->lookup_prev = c->lookup_prev;
    c->lookup_prev = c->lookup_next = NULL;
    c->lookup_waiting = 0;
}

static void ev_conn_close(EvDriver* d, EvConn* c) {
    if (!c) return;
    ev_lookup_unlink(d, c);
    if (c->up.t.sockfd >= 0) {
        aether_io_poller_remove(&d->poller, c->up.t.sockfd);
        ev_untrack(d, c->up.t.sockfd);
        c->up_in_poller = 0;
        http_upstream_release(&c->up, 0);
    }
    if (c->client_fd >= 0) {
        aether_io_poller_remove(&d->poller, c->client_fd);
        ev_untrack(d, c->client_fd);
        c->client_in_poller = 0;
        close(c->client_fd);
    }
    ev_timer_set(d, c, 0);
    ev_conn_reset_request(d, c);
    if (c->req) http_request_free(c->req);
    if (c->res) http_server_response_free(c->res);
    if (c->rxbuf) aether_caps_free(c->rxbuf, c->rxcap);
    if (c->arena_ready) http_arena_free(&c->arena);
#ifdef AETHER_HAS_OPENSSL
    if (c->ssl) {
        /* No SSL_shutdown exchange: this driver never waits, and a close
         * notify that would block is not worth a slot. The peer sees the
         * connection close, which every client handles. */
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#endif
    if (c->head) aether_caps_free(c->head, c->head_cap);
    free(c->out);
    free(c->in);
    free(c);
    atomic_fetch_sub(&d->loop->active, 1);
}

/* A head whose end was just found, judged: the body length it declares, or
 * -1 when the request is the worker path's to answer or decode. */
static long ev_head_verdict(EvConn* c, size_t body_limit) {
    /* The same bound on a head found whole: one read can bring in more than
     * 16 KiB once the buffer has grown for an earlier body, and a head that
     * ended past the bound was taken here, though the worker path refuses
     * it on a fresh connection (#3253). It goes there, which answers 431. */
    if (c->in_hdr_end >= HTTP_CONN_BUF_CAP) return -1;
    char* hdr_end = c->in + c->in_hdr_end - 4;
    /* The worker path's own checks on the head (#3236): the request line's
     * length, each header line's shape, Content-Length and
     * Transfer-Encoding. This driver read Content-Length alone, so it
     * forwarded heads the worker refuses ("Content-Length : 5", an obs-fold
     * line, more headers than the parser holds), and a refusal is the
     * worker path's to answer. So is a chunked body: Transfer-Encoding
     * decides where a body ends, and this driver dropped a chunked one and
     * took Content-Length bytes as the body of a request with both (#3228);
     * the worker path decodes it and bounds it, which keeps one decoder
     * rather than two for a framing proxied clients rarely use. */
    long declared = 0;
    int chunked = 0;
    if (http_request_head_check(c->in, hdr_end, &declared, &chunked) != HTTP_HEAD_OK || chunked)
        return -1;
    /* The body is read whole before anything looks at the request, so a
     * declared length is what this driver will hold: 2 GiB declared, and it
     * set out to buffer 2 GiB (#3237). One longer than the proxy forwards
     * goes to the worker path, which streams a body that size and where the
     * proxy refuses it, 502, without reading it. */
    if ((size_t)declared > body_limit) return -1;
    return declared;
}

/* Is a whole request in the buffer yet?
 *
 * Deliberately the same question the worker path asks, and the answer has to
 * match it: a driver that decided differently would accept a request the
 * other rejects. Headers end at a blank line, then exactly the body the
 * framing declares. 1: a whole request is here; 0: more is needed; -1: its
 * framing is the worker path's to answer or decode (#3228, #3233), or it is
 * larger than this driver buffers (#3237). `body_limit` is the largest body
 * the proxy forwards.
 */
static int ev_request_complete(HttpEvLoop* loop, EvConn* c, size_t body_limit,
                               size_t* out_total) {
    if (c->in_len == 0) return 0;
    c->in[c->in_len] = '\0';

    /* Find the end of the headers once. The search resumes where the last
     * one stopped, backing up three bytes so a terminator split across two
     * reads is still found; without that every read rescans everything read
     * so far. Once found the offset is kept, because a request whose body is
     * still arriving comes back here and a resumed search would look past
     * the terminator. */
    if (!c->in_hdr_end) {
        size_t from = c->in_scanned > 3 ? c->in_scanned - 3 : 0;
        /* memchr for the CR, like the response side already does: strstr pays
         * a two-way-algorithm setup before it looks at a byte, which is a poor
         * trade for a four byte needle, and this runs on every request the
         * client sends. Length-bounded rather than trusting the NUL. */
        const char* found = http_find_header_end(c->in + from, c->in_len - from);
        c->in_scanned = c->in_len;
        if (!found) {
            /* A head that has not ended within the worker path's connection
             * buffer is one that path closes the connection on. This driver
             * grew its buffer for as long as the client kept sending, which a
             * client could do without end (#3237); the worker path takes the
             * connection, with what was read, and does what it does. */
            if (c->in_len + 1 >= HTTP_CONN_BUF_CAP) return -1;
            return 0;
        }
        c->in_hdr_end = (size_t)((found + 4) - c->in);
        c->in_declared = ev_head_verdict(c, body_limit);
    }
    /* The head was judged once, when its end was found; a request whose body
     * is still arriving comes back here on every read (#3269). */
    if (c->in_declared < 0) return -1;
    long declared = c->in_declared;
    size_t header_bytes = c->in_hdr_end;
    /* And no more bodies at once than the worker path holds: one per worker
     * it would run, each up to the cap. Every connection may hold a body
     * the size of the worker's own buffer; a larger one is admitted against
     * the loop's total, or goes to the worker path, which streams it. Held
     * per connection only, the loop buffered the cap times the connections a
     * client cared to open (#3263). */
    if ((size_t)declared > HTTP_CONN_BUF_CAP && !c->body_reserved) {
        long long budget = (long long)loop->body_slots * (long long)body_limit;
        long long held = atomic_fetch_add(&loop->body_held, (long long)declared)
                       + (long long)declared;
        if (held > budget) {
            atomic_fetch_sub(&loop->body_held, (long long)declared);
            return -1;
        }
        c->body_reserved = (size_t)declared;
    }
    if (c->in_len < header_bytes + (size_t)declared) return 0;
    *out_total = header_bytes + (size_t)declared;
    return 1;
}


/* ---- deadlines ----
 *
 * A binary heap ordered by deadline. The loop needs two things from it: the
 * earliest deadline, to decide how long it may wait in poll, and every
 * deadline that has passed. Both are what a heap is for.
 */

static void ev_timer_swap(EvDriver* d, int i, int j) {
    EvConn* a = d->timers[i];
    EvConn* b = d->timers[j];
    d->timers[i] = b; b->heap_pos = i;
    d->timers[j] = a; a->heap_pos = j;
}

static void ev_timer_up(EvDriver* d, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (d->timers[parent]->deadline_ms <= d->timers[i]->deadline_ms) break;
        ev_timer_swap(d, parent, i);
        i = parent;
    }
}

static void ev_timer_down(EvDriver* d, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, small = i;
        if (l < d->timer_count &&
            d->timers[l]->deadline_ms < d->timers[small]->deadline_ms) small = l;
        if (r < d->timer_count &&
            d->timers[r]->deadline_ms < d->timers[small]->deadline_ms) small = r;
        if (small == i) break;
        ev_timer_swap(d, i, small);
        i = small;
    }
}

static void ev_timer_remove(EvDriver* d, EvConn* c) {
    int i = c->heap_pos;
    if (i < 0 || i >= d->timer_count || d->timers[i] != c) { c->heap_pos = -1; return; }
    ev_timer_swap(d, i, d->timer_count - 1);
    d->timer_count--;
    c->heap_pos = -1;
    if (i < d->timer_count) { ev_timer_up(d, i); ev_timer_down(d, i); }
}

static int ev_timer_set(EvDriver* d, EvConn* c, int64_t deadline_ms) {
    ev_timer_remove(d, c);
    c->deadline_ms = deadline_ms;
    if (deadline_ms <= 0) return 0;
    if (d->timer_count == d->timer_cap) {
        int want = d->timer_cap ? d->timer_cap * 2 : 64;
        EvConn** grown = (EvConn**)realloc(d->timers, (size_t)want * sizeof(*grown));
        if (!grown) return -1;
        d->timers = grown;
        d->timer_cap = want;
    }
    d->timers[d->timer_count] = c;
    c->heap_pos = d->timer_count;
    d->timer_count++;
    ev_timer_up(d, d->timer_count - 1);
    return 0;
}

/* How long the loop may wait, given the next deadline. */
static int ev_next_timeout_ms(EvDriver* d, int cap_ms) {
    if (d->timer_count == 0) return cap_ms;
    int64_t wait = d->timers[0]->deadline_ms - aether_proxy_now_ms();
    if (wait < 0) wait = 0;
    return wait < cap_ms ? (int)wait : cap_ms;
}

/* Forward declarations: the states call each other, and a request that can
 * run start to finish without waiting walks most of them in one visit. */
static void ev_arm_deadline(EvDriver* d, EvConn* c);
static int  ev_expire(EvDriver* d, EvConn* c);
static int  ev_hand_back(EvDriver* d, EvConn* c);
static int  ev_hand_to_worker(EvDriver* d, EvConn* c);
static int  ev_watch_upstream(EvDriver* d, EvConn* c);
static void ev_lend_rxbuf(EvConn* c);
static int  ev_respond_from(EvDriver* d, EvConn* c);
static int  ev_begin_upstream(EvDriver* d, EvConn* c);
static int  ev_begin_retry(EvDriver* d, EvConn* c);
static int  ev_upstream_failed(EvDriver* d, EvConn* c, const char* why, int release);
static int  ev_finish_upstream(EvDriver* d, EvConn* c);
static int  ev_advance(EvDriver* d, EvConn* c);

/* ---- the state machine ----
 *
 * Every step either makes progress or says which descriptor it needs to be
 * ready before it can. The driver arms that and goes to another connection,
 * so the thread only sleeps when no connection it owns has anything to do.
 */

/* Register a descriptor once and leave it registered.
 *
 * A one-shot registration has to be armed again after every event, which is a
 * syscall on every wait: measured at 1.33 epoll_ctl per request, work the
 * per-connection path never did. Edge-triggered registration reports a change
 * and stays, so the common path costs nothing after the first call. The state
 * machine already drains every descriptor until it would block, which is what
 * edge triggering requires.
 *
 * Read interest is what stays. Write interest is added only when a write
 * actually blocks and dropped once it drains, because a descriptor that is
 * writable almost always would otherwise wake a level-triggered backend
 * continuously for nothing. */
/* Which of this connection's two descriptors the poller currently holds.
 * Kept here rather than read off the callers' flags, which say what the
 * exchange is waiting for and are set before the registration is attempted. */
static void ev_mark_in_poller(EvConn* c, int fd, int in) {
    if (fd == c->client_fd) c->client_in_poller = in;
    else                    c->up_in_poller = in;
}

static int ev_in_poller(const EvConn* c, int fd) {
    return fd == c->client_fd ? c->client_in_poller : c->up_in_poller;
}

/* On an edge-triggered backend a descriptor is registered once, for both
 * readinesses, and left: EPOLLOUT and EV_CLEAR report a transition, not a
 * state, so an interest nothing waits on never wakes anything. Registering
 * write interest once is what removes the pair of epoll_ctl calls a blocking
 * write used to pay, which the profile put at 1.7% of the driver's time in
 * do_epoll_ctl.
 *
 * A level-triggered backend (poll()) reports a descriptor on every wait for
 * as long as it is ready, so there an interest nothing waits on spins the
 * driver: a writable client after its send had to wait, a client that sent
 * its next request while this one waits on the upstream (#3258). There these
 * calls only remember which connection a descriptor belongs to, and the
 * descriptors' interests are set when the connection parks, to exactly what
 * its state waits on (ev_level_settle). */
static int ev_watch(EvDriver* d, int fd, EvConn* c) {
    if (ev_track(d, fd, c) != 0) return -1;
    if (!d->edge_triggered) return 0;
    ev_mark_in_poller(c, fd, 1);
    return aether_io_poller_add(&d->poller, fd, c,
                                AETHER_IO_READ | AETHER_IO_WRITE | AETHER_IO_EDGE);
}

static int ev_watch_writable(EvDriver* d, int fd, EvConn* c) {
    /* ev_watch asked for write interest already, so a descriptor that is in
     * the poller needs no call here. One that is not in it yet (the upstream,
     * before its first wait) still does. */
    if (d->edge_triggered && ev_in_poller(c, fd)) return 0;
    return ev_watch(d, fd, c);
}

/* Back to watching for readable only, now that the write has drained: on an
 * edge-triggered backend there is nothing to undo, and on a level-triggered
 * one ev_level_settle does it. */
static int ev_unwatch_writable(EvDriver* d, int fd, EvConn* c) {
    (void)d; (void)fd; (void)c;
    return 0;
}

/* One descriptor's interest on a level-triggered backend, `want` 0 for none:
 * registered to stay (AETHER_IO_EDGE, which such a backend keeps
 * level-triggered), changed or removed only when it differs. -1 when the
 * poller would not take it (a full poll set): a connection parked with no
 * interest is never woken, so the caller closes it (#3278). */
static int ev_level_set(EvDriver* d, EvConn* c, int fd, int* in_poller,
                        uint32_t* have, uint32_t want) {
    if (fd < 0) return 0;
    uint32_t cur = *in_poller ? *have : 0;
    if (cur == want) return 0;
    if (!want) {
        aether_io_poller_remove(&d->poller, fd);
        *in_poller = 0;
        *have = 0;
        return 0;
    }
    if (aether_io_poller_add(&d->poller, fd, c, want | AETHER_IO_EDGE) != 0) return -1;
    *in_poller = 1;
    *have = want;
    return 0;
}

/* What a parked connection waits on, as the interest each of its descriptors
 * holds on a level-triggered backend (#3258): the client while a request is
 * read or a handshake carried (readable, or writable when TLS asked to
 * write) and while a response is written (writable, or readable when TLS
 * asked to read); the upstream while it is dialled or written (writable)
 * and read (readable); nothing at all during a backoff, which waits on its
 * timer, or a lookup, which the resolver ends. -1 when the poller would not
 * take an interest, for the caller to close the connection (#3278). */
static int ev_level_settle(EvDriver* d, EvConn* c) {
    uint32_t client = 0, up = 0;
    switch (c->state) {
    case EV_READ_REQUEST:
    case EV_TLS_HANDSHAKE:
        client = c->tls_want == 2 ? AETHER_IO_WRITE : AETHER_IO_READ;
        break;
    case EV_CLIENT_SEND:
        client = c->tls_want == 1 ? AETHER_IO_READ : AETHER_IO_WRITE;
        break;
    case EV_UPSTREAM_DIAL:
    case EV_UPSTREAM_SEND:
        up = AETHER_IO_WRITE;
        break;
    case EV_UPSTREAM_RECV:
        up = AETHER_IO_READ;
        break;
    default:
        break;
    }
    if (ev_level_set(d, c, c->client_fd, &c->client_in_poller, &c->client_events,
                     client) != 0)
        return -1;
    return ev_level_set(d, c, c->up.t.sockfd, &c->up_in_poller, &c->up_events, up);
}



/* Hand the connection's accumulator to the exchange about to fill it. The
 * exchange grows it as it needs; the connection takes it back when the
 * request finishes, so a busy connection allocates one and reuses it. */
static void ev_lend_rxbuf(EvConn* c) {
    if (!c->rxbuf) return;
    c->x.buf = c->rxbuf;
    c->x.cap = c->rxcap;
    c->x.len = 0;
    c->rxbuf = NULL;
    c->rxcap = 0;
}

/* Register the upstream the first time this request has to wait on it, and
 * not before. A request whose answer is already there never registers it at
 * all, which is the common case against a fast upstream. */
/* Take the accumulator back from the exchange, which is what ev_begin_retry
 * needs before it clears the exchange: the buffer is lent, not owned, and a
 * memset over the exchange would drop the only pointer to it. */
static void ev_reclaim_rxbuf(EvConn* c) {
    if (!c->x.buf) return;
    if (c->rxbuf) aether_caps_free(c->rxbuf, c->rxcap);
    c->rxbuf = c->x.buf;
    c->rxcap = c->x.cap;
    c->x.buf = NULL;
    c->x.cap = 0;
    c->x.len = 0;
}

/* Close and forget the upstream, without pooling it. It is being abandoned
 * because it failed, and a failed descriptor must never go back into the pool
 * for the next request to inherit. */
static void ev_drop_upstream(EvDriver* d, EvConn* c) {
    if (c->up.t.sockfd < 0) return;
    if (c->up_watched || c->up_in_poller) aether_io_poller_remove(&d->poller, c->up.t.sockfd);
    ev_untrack(d, c->up.t.sockfd);
    c->up_in_poller = 0;
    c->up_watched = c->up_writable_watched = 0;
    http_upstream_release(&c->up, 0);
}

static int ev_watch_upstream(EvDriver* d, EvConn* c) {
    if (c->up_watched) return 0;
    c->up_watched = 1;
    return ev_watch(d, c->up.t.sockfd, c) == 0 ? 0 : -1;
}

/* Client I/O, over TLS or not.
 *
 * Both return what a socket call would: bytes moved, or -1 with errno set to
 * EAGAIN when the caller should wait. OpenSSL reports the same condition as
 * SSL_ERROR_WANT_READ or WANT_WRITE, and a TLS record can need the socket
  * readable to finish a write or writable to finish a read; the driver keeps
 * both interests registered on an edge-triggered backend, so either wake
 * brings it back here. A level-triggered one waits on the one asked for,
 * which tls_want carries (#3258).
 */
static ssize_t ev_client_read(EvConn* c, void* buf, size_t n) {
#ifdef AETHER_HAS_OPENSSL
    if (c->ssl) {
        ERR_clear_error();
        c->tls_want = 0;
        int r = SSL_read(c->ssl, buf, (int)n);
        if (r > 0) return r;
        int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            c->tls_want = e == SSL_ERROR_WANT_WRITE ? 2 : 1;   /* #3258 */
            errno = EAGAIN;
            return -1;
        }
        if (e == SSL_ERROR_ZERO_RETURN) return 0;   /* the peer closed cleanly */
        errno = EIO;
        return -1;
    }
#endif
    return recv(c->client_fd, buf, n, 0);
}

#ifdef AETHER_HAS_OPENSSL
/* A write to a TLS client. Only the TLS branch of ev_step_client_send writes
 * one segment at a time; a plain client is written with sendmsg there, so a
 * build without OpenSSL has no use for this and defines none (#3271). */
static ssize_t ev_client_write(EvConn* c, const void* buf, size_t n) {
    ERR_clear_error();
    c->tls_want = 0;
    int r = SSL_write(c->ssl, buf, (int)n);
    if (r > 0) return r;
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        c->tls_want = e == SSL_ERROR_WANT_WRITE ? 2 : 1;   /* #3258 */
        errno = EAGAIN;
        return -1;
    }
    errno = EIO;
    return -1;
}
#endif

#ifdef AETHER_HAS_OPENSSL
/* Carry the handshake as far as it will go without waiting.
 *
 * Returns 1 when it is done, 0 when the descriptor has to be waited on, and
 * -1 when the peer is not speaking TLS this way -- which is ordinary traffic,
 * not an error worth logging: a plain-HTTP request to a TLS port arrives here
 * and the connection is simply closed.
 */
static int ev_step_tls_handshake(EvDriver* d, EvConn* c) {
    ERR_clear_error();
    int r = SSL_accept(c->ssl);
    if (r == 1) {
        c->tls_want = 0;
        c->state = EV_READ_REQUEST;
        return 1;
    }
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        c->tls_want = e == SSL_ERROR_WANT_WRITE ? 2 : 1;   /* #3258 */
        return ev_watch_writable(d, c->client_fd, c) == 0 ? 0 : -1;
    }

    /* Drain the error queue, or the next handshake on this thread starts with
     * someone else's failure still on it. */
    ERR_clear_error();
    return -1;
}
#endif

/* Read whatever the client has, without waiting for it. */
static int ev_step_read_request(EvDriver* d, EvConn* c) {
    for (;;) {
        /* Parse what is already buffered before asking the kernel for more:
         * after a pipelined request the next one is here already, and reading
         * first would block it behind bytes that never come. */
        if (c->in_len > 0) {
            /* The request began with its first byte: the request hooks'
             * duration runs from here, as the worker path's runs from
             * before its read (#3214). Not read when no hook will ask. */
            if (!c->t_start_us && d->loop->server->request_hook_chain)
                c->t_start_us = http_server_clock_us();
            size_t total = 0;
            http_server_loop_plan(d->loop->server, &d->plan);   /* #3269 */
            AetherProxyOpts* opts = (AetherProxyOpts*)d->plan.proxy_opts;
            size_t body_limit = opts ? (size_t)opts->max_body_bytes : 0;
            int complete = ev_request_complete(d->loop, c, body_limit, &total);
            /* Framing this driver cannot resolve: the worker path has the
             * answer for it (a 400 for a length that is no length), so the
             * connection goes there with the bytes read, rather than being
             * closed with nothing written (#3233). */
            if (complete < 0) return ev_hand_back(d, c);
            if (complete) {                      /* a whole request is here */
                c->in_total = total;
                return 1;
            }
        }

        if (ev_in_reserve(c, 4096) != 0) return -1;
        ssize_t n = ev_client_read(c, c->in + c->in_len,
                                   c->in_cap - c->in_len - 1);
        if (n > 0) {
            c->in_len += (size_t)n;
            continue;
        }
        if (n == 0) return -1;                   /* the client is finished */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;      /* already watched; the next change wakes us */
        if (errno == EINTR) continue;
        return -1;
    }
}

/* Write what is owed to the client, without waiting for it to drain. */
static int ev_step_client_send(EvDriver* d, EvConn* c) {
    for (;;) {
        struct iovec iov[2];
        int n_iov = 0;
        if (c->out_sent < c->out_len) {
            iov[n_iov].iov_base = c->out + c->out_sent;
            iov[n_iov].iov_len  = c->out_len - c->out_sent;
            n_iov++;
        }
        if (c->out_body && c->out_body_sent < c->out_body_len) {
            iov[n_iov].iov_base = (void*)(c->out_body + c->out_body_sent);
            iov[n_iov].iov_len  = c->out_body_len - c->out_body_sent;
            n_iov++;
        }
        if (n_iov == 0) return 1;

        ssize_t w;
#ifdef AETHER_HAS_OPENSSL
        if (c->ssl) {
            /* TLS has no scatter write: the two segments go out in order, and
             * the loop comes back for the second once the first is away. */
            w = ev_client_write(c, iov[0].iov_base, iov[0].iov_len);
        } else
#endif
        {
            /* One call for both, so a pass-through does not pay an extra send
             * to put the body on the wire after the head, and does not have to
             * copy the body into the head's buffer to avoid that.
             *
             * sendmsg rather than writev because it takes flags: a peer that
             * went away raises SIGPIPE on the write otherwise, and the default
             * disposition for that is to kill the process. The server ignores
             * SIGPIPE when it starts, and this asks for the same per call so
             * an embedder that sets its own disposition is still safe. */
            if (n_iov == 1) {
                /* One segment needs no scatter list. send() skips the msghdr
                 * the kernel would otherwise have to walk, and takes the same
                 * flags. */
                w = send(c->client_fd, iov[0].iov_base, iov[0].iov_len,
                         AE_SEND_NOSIGNAL);
            } else {
                struct msghdr msg;
                memset(&msg, 0, sizeof(msg));
                msg.msg_iov    = iov;
                msg.msg_iovlen = (size_t)n_iov;
                w = sendmsg(c->client_fd, &msg, AE_SEND_NOSIGNAL);
            }
        }
        if (w > 0) {
            size_t left = (size_t)w;
            size_t head_left = c->out_len - c->out_sent;
            size_t took = left < head_left ? left : head_left;
            c->out_sent += took;
            left -= took;
            if (left) c->out_body_sent += left;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return ev_watch_writable(d, c->client_fd, c) == 0 ? 0 : -1;
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
}

/* Push the request at the upstream. The exchange is the same one the blocking
 * client drives; on a non-blocking descriptor it reports what it is waiting
 * for instead of waiting. */
static int ev_step_upstream_send(EvDriver* d, EvConn* c) {
    int r = http_exchange_send(&c->x);
    if (r == AE_X_DONE) {
        /* Written; only the answer is awaited now. */
        if (c->up_writable_watched) {
            ev_unwatch_writable(d, c->up.t.sockfd, c);
            c->up_writable_watched = 0;
        }
        return 1;
    }
    if (r == AE_X_WANT_WRITE) {
        if (c->up_writable_watched) return 0;
        c->up_writable_watched = 1;
        c->up_watched = 1;
        return ev_watch_writable(d, c->up.t.sockfd, c) == 0 ? 0 : -1;
    }
    if (r == AE_X_WANT_READ) return ev_watch_upstream(d, c);
    return -1;
}

static int ev_step_upstream_recv(EvDriver* d, EvConn* c) {
    int r = http_exchange_recv(&c->x);
    if (r == AE_X_DONE) return 1;
    if (r == AE_X_WANT_READ) return ev_watch_upstream(d, c);
    if (r == AE_X_WANT_WRITE) {
        if (c->up_writable_watched) return 0;
        c->up_writable_watched = 1;
        c->up_watched = 1;
        return ev_watch_writable(d, c->up.t.sockfd, c) == 0 ? 0 : -1;
    }
    return -1;
}



/* A response object to send: the proxy's (an upstream's answer it copied, a
 * cache hit, a refusal, a 502 or 504 of its own) or a middleware's ahead of
 * it, a 429 from the rate limiter, a CORS preflight, an unknown host
 * (#3214). It goes out as the worker path sends one
 * (http_server_answer_bytes): the request hooks observe it first, as they
 * observe every response the server sends (#3213); the keep-alive decision
 * and the headers stating it are the ones finish_response makes (#3229); a
 * HEAD carries no body, which the proxy's own answers did, so a client read
 * the body as the next response (#3251). When the decision is to close (the
 * client said so, HTTP/1.0, the server's keep-alive policy), the connection
 * ends once the response is written, as it does there. */
static int ev_answer(EvDriver* d, EvConn* c);
static int ev_respond_from(EvDriver* d, EvConn* c) {
    return ev_answer(d, c);
}

static int ev_answer(EvDriver* d, EvConn* c) {
    size_t len = 0;
    int keep = http_server_answer_bytes(d->loop->server, c->req, c->res, c->t_start_us,
                                        c->requests_served, &c->out, &c->out_cap, &len);
    if (keep < 0) return -1;
    c->out_len = len;
    c->out_sent = 0;
    c->close_after = !keep;
    c->state = EV_CLIENT_SEND;
    return 1;
}

/* The proxy asked for another attempt, against whichever upstream it picked
 * this time. Same path as the first attempt: a connection, a head, an
 * exchange. */
static int ev_dial(EvDriver* d, EvConn* c, int next_addr);
static int ev_begin_retry(EvDriver* d, EvConn* c) {
    /* A retry's backoff is waited out on a timer (#3232). The exchange used
     * to sleep it, on this thread, and every connection the driver held
     * waited with it; now the connection parks and ev_expire dials when it
     * is due. */
    if (c->px.backoff_ms > 0) {
        int64_t due = aether_proxy_now_ms() + c->px.backoff_ms;
        c->px.backoff_ms = 0;
        c->state = EV_BACKOFF;
        return ev_timer_set(d, c, due) == 0 ? 0 : -1;
    }
    return ev_dial(d, c, 0);
}

/* The upstream's name has no answer yet (#3267). The loop's resolver is
 * looking it up on a thread of its own, and the connection waits here,
 * holding nothing the driver needs, until the answer arrives
 * (ev_resume_lookups) or the request's deadline passes (ev_expire). The
 * driver goes on with every other connection meanwhile: it used to look the
 * name up itself, and they all waited with it. The dial goes again from the
 * start, a fresh one if this one was to be. The upstream deadline starts
 * with the first wait and is not moved by another (#3283): the dial after
 * it keeps it, so the request's upstream timeout covers the lookup. */
static int ev_lookup_wait(EvDriver* d, EvConn* c, int fresh, unsigned name) {
    c->state = EV_UPSTREAM_LOOKUP;
    c->dial_fresh = fresh;
    c->lookup_name = name;
    c->lookup_prev = NULL;
    c->lookup_next = d->lookups;
    if (d->lookups) d->lookups->lookup_prev = c;
    d->lookups = c;
    c->lookup_waiting = 1;
    if (!c->lookup_deadline) {
        ev_arm_deadline(d, c);
        c->lookup_deadline = 1;
    }
    return 0;
}

/* The proxy picked an upstream; dial it, or take one from the pool, and get
 * the head and the exchange ready to send: a request's first attempt, a
 * retry, and a dial a lookup held up all come here. `next_addr` 1: the
 * connect to one of the upstream's addresses failed and it has another, so
 * dial that rather than starting over. */
static int ev_dial(EvDriver* d, EvConn* c, int next_addr) {
    const char* url = http_request_url_of(c->px.outbound);
    /* The path is sized from the URL; a fixed path[1024] cut a long client
     * path, and the proxy forwarded the cut one (#2674). */
    char host[256];
    char* path = NULL;
    int port = 0, use_tls = 0;
    if (!url || !parse_url_alloc(url, host, sizeof(host), &port, &path, &use_tls))
        return -1;
    /* this driver dials plain HTTP only: an https upstream is the worker
     * path's (#3231) */
    if (use_tls) { free(path); return ev_hand_to_worker(d, c); }

    int fresh = c->dial_fresh;
    c->dial_fresh = 0;
    int fd = next_addr ? http_upstream_redial(host, port, &c->up)
                       : http_upstream_acquire_nb(host, port, !fresh, d->loop->resolver,
                                                  &c->up);
    if (fd < 0) {
        free(path);
        /* Never a name lookup on this thread (#3267): the resolver has it in
         * hand, or the name does not resolve, which the exchange answers as
         * the worker path's dial would. */
        int lookup = http_upstream_lookup(&c->up);
        if (lookup > 0) return ev_lookup_wait(d, c, fresh, http_resolver_name(host, port));
        return ev_upstream_failed(d, c, lookup < 0 ? "could not resolve host"
                                                   : "connection failed", 0);
    }

    int body_len = 0;
    const char* body = http_request_body_of(c->px.outbound, &body_len);
    /* Rebuilt in place rather than released and taken again. The buffer
     * belongs to the connection now, and is returned through the cap when the
     * connection ends -- which is what the accounting requires, and what a
     * plain free() here used to get wrong. */
    c->head_len = 0;
    HttpReqHead head_params = {
        c->px.outbound, http_request_method_of(c->px.outbound), path,
        host, port, 0, 0, body, body_len,
        http_request_content_type_of(c->px.outbound), 1
    };
    c->head = http_build_request_head_into(&head_params, &c->head, &c->head_cap,
                                           &c->head_len);
    free(path);   /* the head holds the request line now */
    if (!c->head) return -1;

    /* The receive buffer may still be lent to the attempt this one replaces:
     * a 5xx with retries left comes here from ev_finish_upstream with it in
     * the exchange. Clearing the exchange dropped the only pointer to it,
     * 16 KiB or more per retry (#3249). */
    ev_reclaim_rxbuf(c);
    memset(&c->x, 0, sizeof(c->x));
    http_exchange_init(&c->x, &c->up.t, c->head, c->head_len, body, body_len,
                       http_request_method_of(c->px.outbound));
    ev_lend_rxbuf(c);
    /* A dial a lookup held up keeps the deadline its wait armed (#3283). */
    if (c->lookup_deadline) c->lookup_deadline = 0;
    else                    ev_arm_deadline(d, c);
    if (c->up.connecting) {
        c->state = EV_UPSTREAM_DIAL;
        c->up_writable_watched = 1;
        c->up_watched = 1;
        return ev_watch_writable(d, fd, c) == 0 ? 0 : -1;
    }
    c->up_writable_watched = 0;
    c->up_watched = 0;
    c->state = EV_UPSTREAM_SEND;
    return ev_track(d, fd, c) == 0 ? 1 : -1;
}



/* Stop tracking the client here, for the general server path to take it.
 *
 * Ownership moves whole: the descriptor leaves this driver's poller and its
 * table before the other path sees it, so no event can arrive for a
 * connection this driver no longer owns. Returns the descriptor; the TLS
 * session, if any, goes to *ssl.
 */
static int ev_release_client(EvDriver* d, EvConn* c, void** ssl) {
    int fd = c->client_fd;
    aether_io_poller_remove(&d->poller, fd);
    ev_untrack(d, fd);
    c->client_in_poller = 0;
    ev_timer_set(d, c, 0);
    c->client_fd = -1;           /* ev_conn_close must not close it now */

    /* The session goes with it, and this connection stops owning it: the
     * handshake is done and the socket carries ciphertext, so a worker handed
     * only the descriptor would read bytes it cannot decode. */
    *ssl = NULL;
#ifdef AETHER_HAS_OPENSSL
    *ssl = c->ssl;
    c->ssl = NULL;
#endif
    return fd;
}

/* The other path could not take it: nobody owns it now, so it closes. */
static void ev_release_failed(int fd, void* ssl) {
#ifdef AETHER_HAS_OPENSSL
    if (ssl) SSL_free((SSL*)ssl);
#else
    (void)ssl;
#endif
    close(fd);
}

/* Give the connection to the general server path, with nothing of the
 * current request run here. A pool worker serves it from then on, as it does
 * an accepted connection: nothing of the worker path runs on this thread
 * (#3230). */
static int ev_hand_back(EvDriver* d, EvConn* c) {
    void* ssl = NULL;
    int fd = ev_release_client(d, c, &ssl);
    /* Every buffered byte goes with it, not just this request's: a client
     * may have pipelined the next one into the same segment, and the worker
     * taking over is the only thing that will ever read it. So does the
     * count of responses this driver sent on it, which the server's
     * keep_alive_max goes on counting from (#3229). */
    if (http_server_adopt_request(d->loop->server, fd, ssl, NULL, NULL, 0,
                                  c->requests_served, 0, c->in, (int)c->in_len) != 0)
        ev_release_failed(fd, ssl);
    return -1;                   /* finished with, as far as this driver goes */
}

/* Give the connection to the general server path after the middleware
 * ahead of the proxy ran on its request and the proxy did not take it
 * (#3214): the worker path goes on from the middleware after the mount, with
 * the request as the chain left it (http_server_adopt_request). Handing back
 * the bytes instead would run the chain on it twice: a rate limiter would
 * count it twice. What was read past this request goes with it. With
 * `at_proxy` 1 the worker path goes on from the mount itself (#3231). */
static int ev_hand_over_from(EvDriver* d, EvConn* c, int at_proxy) {
    void* ssl = NULL;
    int fd = ev_release_client(d, c, &ssl);
    if (http_server_adopt_request(d->loop->server, fd, ssl, c->req, c->res,
                                  c->t_start_us, c->requests_served, at_proxy,
                                  c->in + c->in_total,
                                  (int)(c->in_len - c->in_total)) == 0) {
        c->req = NULL;           /* the worker path's now, and done with */
        c->res = NULL;
    } else {
        ev_release_failed(fd, ssl);
    }
    return -1;
}

static int ev_hand_over(EvDriver* d, EvConn* c) {
    return ev_hand_over_from(d, c, 0);
}

/* The proxy picked an upstream this driver cannot dial: an https one. It
 * has no TLS client, and it used to close the client's connection with
 * nothing written (#3231); a server with one is not served here at all
 * (http_server_evloop_serves), so one added while this request was on its
 * way, or picked by a retry, lands here. The pick goes back, and the request
 * goes to the worker path: from the mount itself when the middleware ahead
 * of it already ran here, else with its bytes, as if this driver had never
 * read it. */
static int ev_hand_to_worker(EvDriver* d, EvConn* c) {
    aether_proxy_exchange_abandon(&c->px);
    ev_release_attempt(c);
    if (!c->before_proxy) return ev_hand_back(d, c);
    return ev_hand_over_from(d, c, 1);
}

/* The proxy's configured upstream timeout, as a moment to give up. A proxy
 * that waits forever on an upstream is not one anybody can run: the blocking
 * path gets this from the socket, and a driver that never blocks has to keep
 * the time itself. */
static void ev_arm_deadline(EvDriver* d, EvConn* c) {
    AetherProxyOpts* opts = (AetherProxyOpts*)d->plan.proxy_opts;   /* current: ev_begin_upstream */
    int secs = (opts && opts->pool) ? opts->pool->request_timeout_sec : 0;
    if (secs <= 0) { ev_timer_set(d, c, 0); return; }
    ev_timer_set(d, c, aether_proxy_now_ms() + (int64_t)secs * 1000);
}

/* How long this driver waits on a quiet client: the worker path's receive
 * timeout (conn_apply_recv_timeout), the server's keep-alive idle timeout,
 * 30 seconds unless set. Armed while the driver waits on the client for a
 * request, and moved each time bytes of it arrive, as a receive timeout
 * restarts with each receive; a TLS handshake has it once, start to finish,
 * and the request after it starts afresh. With no timeout at all, a client held
 * a connection for as long as it kept it open, idle or halfway through a
 * request (#3243). The same bound covers writing a response to a client that
 * stopped reading, moved each time it takes bytes, as the worker path's send
 * timeout (conn_apply_send_timeout) restarts with each send (#3257).
 *
 * Kept as a time on the connection rather than in the deadline heap: a kept
 * connection waits on its client between every two requests, and a heap
 * insertion and removal for each was work on every request for a timeout
 * that almost never fires (#3269). ev_sweep_idle closes the ones past it,
 * a few times a second. */
static void ev_arm_idle(EvDriver* d, EvConn* c) {
    int ms = d->loop->server->keep_alive_idle_ms > 0
           ? d->loop->server->keep_alive_idle_ms : 30000;
    c->idle_due = aether_proxy_now_ms() + ms;
}

/* How often ev_sweep_idle looks, and so how late past its idle timeout a
 * quiet connection may close. */
#define EV_IDLE_SWEEP_MS 250

/* Close every connection whose client has been quiet past its idle timeout
 * (ev_arm_idle): one waiting on a request or a handshake, as the worker
 * path closes on its receive timeout (#3243), or on a client that stopped
 * reading its response, as the worker path's send fails on its send timeout
 * (#3257). */
static void ev_sweep_idle(EvDriver* d, int64_t now) {
    if (now < d->next_idle_sweep_ms) return;
    d->next_idle_sweep_ms = now + EV_IDLE_SWEEP_MS;
    for (int fd = 0; fd < d->by_fd_cap; fd++) {
        EvConn* c = d->by_fd[fd];
        if (!c || c->client_fd != fd || !c->idle_due || c->idle_due > now) continue;
        if (c->state == EV_READ_REQUEST || c->state == EV_TLS_HANDSHAKE ||
            c->state == EV_CLIENT_SEND)
            ev_conn_close(d, c);
    }
}

/* A deadline in the heap has passed: a retry's backoff is over, or the
 * upstream did not answer in time. */
static int ev_expire(EvDriver* d, EvConn* c) {
    ev_timer_set(d, c, 0);
    /* Not a timeout: the backoff before a retry is over (#3232). */
    if (c->state == EV_BACKOFF) return ev_dial(d, c, 0);
    /* The upstream's name took longer to look up than the request may wait:
     * a timeout like any other, from the exchange, and no longer a waiter. */
    if (c->state == EV_UPSTREAM_LOOKUP) {
        ev_lookup_unlink(d, c);
        return ev_upstream_failed(d, c, "request timed out before the response arrived", 0);
    }
    /* The upstream did not answer in time: a failed call, which goes back to
     * the proxy exchange as the worker path's driver sends its own (#3250).
     * The exchange decrements the upstream's in-flight count, tells its
     * breaker, counts the timeout and retries where it may, else answers
     * 504. Answered here instead, an upstream with an in-flight cap stayed
     * full after its first timeout and a breaker never opened on timeouts.
     * The words are the blocking client's for a deadline, which the exchange
     * reads as a timeout. The connection is dropped, never pooled mid-answer. */
    return ev_upstream_failed(d, c, c->x.framing.header_bytes
                                    ? "request timed out before the response ended"
                                    : "request timed out before the response arrived", 1);
}

/* ---- the bridge to the proxy ----
 *
 * The proxy's own semantics (routing, upstream choice, retries, the breaker,
 * the cache, header rewriting) live in one resumable exchange that the
 * blocking path drives too. This driver supplies the send, which is the only
 * part that differs, so the two cannot drift apart on what a proxied request
 * means.
 */

/* The upstream could not be had, or failed before it answered. The client is
 * owed an answer all the same, and it is the blocking path's: the failure goes
 * to the proxy exchange as a response with an error, which counts it against
 * the upstream, retries where it may (another upstream, or the same again) and
 * otherwise makes the 502 or 504. Returning -1 here instead closed the client's
 * connection with nothing written. `release` is 1 when an upstream connection
 * was taken and has to be given up first. */
static int ev_upstream_failed(EvDriver* d, EvConn* c, const char* why, int release) {
    ev_timer_set(d, c, 0);
    c->lookup_deadline = 0;   /* the attempt is over (#3283) */
    if (release) ev_drop_upstream(d, c);
    ev_reclaim_rxbuf(c);
    HttpResponse* resp = http_response_alloc_failed(why);
    if (!resp) return -1;
    c->px.resp = resp;
    ev_release_attempt(c);   /* over; resume may build the next (#3242) */
    if (aether_proxy_exchange_resume(&c->px, 0) == PX_NEED_SEND) {
        return ev_begin_retry(d, c);
    }
    return ev_respond_from(d, c);
}

/* Parse the request at the front of `in`, `arena` NULL for the ordinary
 * allocator. The bytes after it are the next request's, so the parse is
 * bounded the way the worker path bounds it, by a NUL at the request's end:
 * a request with no Content-Length had everything buffered after it, the
 * next pipelined request included, parsed as its body, and the proxy
 * forwarded that body upstream. */
static int ev_parse_request(EvConn* c, HttpArena* arena) {
    char saved = c->in[c->in_total];     /* in_total < in_cap (ev_in_reserve) */
    c->in[c->in_total] = '\0';
    HttpRequest* parsed = arena
        ? http_parse_request_arena(c->req, c->in, c->in_total, arena)
        : http_parse_request_into(c->req, c->in, c->in_total);
    c->in[c->in_total] = saved;
    return parsed != NULL;
}

/* The request is complete: run what precedes the proxy, and the proxy up to
 * the point where it needs the upstream, then get a connection and a
 * serialised head ready to go. */
static int ev_begin_upstream(EvDriver* d, EvConn* c) {
    HttpServer* server = d->loop->server;
    /* A server changed after this connection was taken may have gained a
     * middleware ahead of the proxy, or a request hook, that is not
     * loop-safe (#3214): from this request on the worker path serves the
     * connection, and nothing of the request has run here yet. */
    if (!http_server_loop_plan(server, &d->plan)) return ev_hand_back(d, c);

    if (!c->meta_ready) {
        /* The connection's peer, which the proxy names in X-Forwarded-For
         * (#3169), and its TLS state, which a middleware may ask (#3214). */
        int is_tls = 0;
#ifdef AETHER_HAS_OPENSSL
        is_tls = c->ssl != NULL;
#endif
        http_conn_meta_from_fd(&c->meta, c->client_fd, is_tls);
        c->meta_ready = 1;
    }
    c->before_proxy = d->plan.before_proxy;
    c->hooked = server->request_hook_chain != NULL;
    if (c->hooked && !c->t_start_us) c->t_start_us = http_server_clock_us();

    /* The arena is emptied before the parse, not after it: the parse takes
     * this request's strings from it, and resetting afterwards would hand the
     * proxy pointers into a block that had just been handed back. It holds
     * both the inbound request's strings and the outbound request's headers,
     * so it is sized for two requests rather than one. */
    if (!c->arena_ready) {
        c->arena_ready = http_arena_init(&c->arena, 16384) == 0;
    } else {
        http_arena_reset(&c->arena);
    }

    /* A request that does not parse goes to the worker path with the bytes
     * read, which answers it 400 (or 414 for a request line too long to
     * hold), as it answers every request it cannot parse; closing the
     * connection here left the client with no status at all (#3233). */
    if (c->before_proxy) {
        /* A middleware may change the request it is given, by the contract
         * the worker path gives it: a request of its own, whose strings are
         * each a malloc. use_real_ip replaces its peer; a program's middleware
         * may set any field. A peer replaced on one request and reported by
         * the connection's next, or a program's edit of an arena string,
         * would break that, so this request is parsed outside the arena into
         * an object of its own, which goes with it (ev_conn_reset_request).
         * std's own edits go through the server's request edits, which keep
         * an arena-backed or reused request whole (#3234). */
        if (c->req) http_request_free(c->req);
        c->req = (HttpRequest*)calloc(1, sizeof(HttpRequest));
        if (!c->req) return -1;
        if (!ev_parse_request(c, NULL)) return ev_hand_back(d, c);
        http_request_set_conn_meta(c->req, &c->meta);
    } else {
        if (!c->req) {
            c->req = (HttpRequest*)calloc(1, sizeof(HttpRequest));
            if (!c->req) return -1;
            /* One request object per connection, which a parse resets
             * without touching these. */
            http_request_set_conn_meta(c->req, &c->meta);
        }
        if (!ev_parse_request(c, c->arena_ready ? &c->arena : NULL)) return ev_hand_back(d, c);
    }

    if (!c->res) {
        c->res = http_response_create();
        if (!c->res) return -1;
    } else {
        http_response_reset(c->res);
    }

    /* The middleware ahead of the proxy, as the worker path's chain runs it
     * (#3214): each loop-safe, or this server would not be served here. */
    if (c->before_proxy && !http_server_run_before_proxy(server, c->req, c->res))
        return ev_answer(d, c);   /* 1: the client write is next */

    void* opts = d->plan.proxy_opts;
    int r = aether_proxy_exchange_begin(&c->px, c->req, c->res, opts,
                                        c->arena_ready ? &c->arena : NULL);
    if (r == 1) {
        /* Not the proxy's request: a health endpoint, an admin route, or
         * anything another middleware answers. This driver knows one kind of
         * work, so the rest goes back to the path that knows the others,
         * carrying the bytes already read off the socket; or, when the
         * middleware ahead of the proxy ran on it, the request as it left
         * them. */
        return c->before_proxy ? ev_hand_over(d, c) : ev_hand_back(d, c);
    }
    if (r != PX_NEED_SEND) {
        /* Served without an upstream: a cache hit or a refusal. The response
         * is already filled in. */
        return ev_respond_from(d, c);   /* 1: the client write is next */
    }
    /* The upstream, through the same dial a retry takes (#3267). */
    return ev_dial(d, c, 0);
}

/* Append to the response head being built in `out`, growing it as the
 * proxy's direct head does. */
static int ev_out_put(EvConn* c, size_t* len, const char* s, size_t n) {
    if (*len + n + 1 > c->out_cap) {
        size_t want = c->out_cap ? c->out_cap : 1024;
        while (want < *len + n + 1) want *= 2;
        char* grown = (char*)realloc(c->out, want);
        if (!grown) return -1;
        c->out = grown;
        c->out_cap = want;
    }
    memcpy(c->out + *len, s, n);
    *len += n;
    c->out[*len] = '\0';
    return 0;
}

/* The upstream has answered. Hand the reply back to the proxy exchange, which
 * decides whether it is the one to keep, and turn what it produced into bytes
 * for the client. */
/* Answer straight from the bytes the upstream sent.
 *
 * The copying path materialises a response three times: into an HttpResponse,
 * then header by header onto the HttpServerResponse, then into the bytes that
 * go out. The last two exist so a handler could have looked at it. When
 * nothing will, the bytes already in the receive buffer are the answer: the
 * head is rewritten into `out` and the body is sent from where it landed.
 *
 * Only for a response whose own framing said where it ended, and which is not
 * chunked -- a chunked body would have to be decoded to be forwarded, which is
 * a copy again. Everything else falls back, so this narrows what it handles
 * rather than changing what the proxy means.
 *
 * Returns 1 when it answered, 0 when the caller should take the copying path,
 * -1 when the head could not be finished and the connection has to close.
 */
static int ev_try_direct(EvDriver* d, EvConn* c) {
    if (!aether_proxy_direct_ok(&c->px))                     return 0;
    if (!c->x.complete || c->x.framing.invalid)              return 0;
    if (c->x.framing.chunked || !c->x.framing.definite)      return 0;
    if (!c->x.buf || c->x.framing.header_bytes == 0)         return 0;
    /* Response transformers are the route handler's: a proxied response is
     * not one of theirs on the worker path either (#3213), so the upstream's
     * own bytes are the answer whether one is registered or not. The request
     * hooks are another matter (#3214): they observe the response object,
     * which this path never builds, and a middleware ahead of the proxy may
     * have put headers on that object (CORS does) that the upstream's bytes
     * do not carry. Either takes the copying path, the worker path's. */
    if (c->hooked || c->before_proxy)                        return 0;

    AetherProxyDirect direct;
    if (aether_proxy_direct_take(&c->px, 0, c->x.buf, c->x.len,
                                 c->x.framing.header_bytes, &direct) != 0) return 0;

    /* The response is recorded from here on (metrics, breaker, in-flight), so
     * a head that cannot be written closes rather than falling back to the
     * copying path, which would record it a second time (#3244). */
    size_t head_len = 0;
    if (aether_proxy_direct_head(&direct, c->x.buf, c->x.framing.header_bytes,
                                 &c->out, &c->out_cap, &head_len) != 0) return -1;

    /* The keep-alive decision and the lines stating it, which the copying
     * path's response carries after every other header (#3229): they go
     * before the blank line that ends the head, and fail the same way. */
    char lines[160];
    size_t lines_len = 0;
    int keep = http_server_keep_alive_lines(d->loop->server, c->req, direct.status,
                                            c->requests_served, lines, sizeof(lines),
                                            &lines_len);
    head_len -= 2;
    if (ev_out_put(c, &head_len, lines, lines_len) != 0 ||
        ev_out_put(c, &head_len, "\r\n", 2) != 0) return -1;
    c->close_after = !keep;

    c->out_len       = head_len;
    c->out_sent      = 0;
    c->out_body      = direct.body;
    c->out_body_len  = direct.body_len;
    c->out_body_sent = 0;

    /* A body small enough to copy joins the head, so the response leaves in
     * one send() rather than a two-segment sendmsg. The copy is a few dozen
     * bytes; the scatter list it removes is a msghdr the kernel walks on every
     * response. A large body still goes out from where it landed, which is the
     * whole point of the pass-through. */
    if (direct.body && direct.body_len > 0 && direct.body_len <= 2048 &&
        head_len + (size_t)direct.body_len + 1 <= c->out_cap) {
        memcpy(c->out + head_len, direct.body, (size_t)direct.body_len);
        c->out_len      = head_len + (size_t)direct.body_len;
        c->out_body     = NULL;
        c->out_body_len = 0;
    }

    c->state = EV_CLIENT_SEND;
    return 1;
}

static int ev_finish_upstream(EvDriver* d, EvConn* c) {
    ev_timer_set(d, c, 0);       /* the upstream answered; nothing to give up on */

    /* The connection is worth keeping only when the response ended where its
     * own framing said it would, and did not say the connection closes
     * (Connection: close, or HTTP/1.0 without keep-alive), as the worker
     * path's client decides: pooled anyway, the next request took a
     * connection the upstream had closed (#3259). Released before the
     * response is looked at, because nothing about releasing it depends on
     * that. */
    int keep = c->x.complete && !c->x.framing.invalid &&
               c->x.framing.header_bytes > 0 &&
               http_response_head_keeps_conn(c->x.buf, c->x.framing.header_bytes);
    int was_reused = c->up.reused;
    size_t got = c->x.len;
    if (c->up_watched || c->up_in_poller) aether_io_poller_remove(&d->poller, c->up.t.sockfd);
    ev_untrack(d, c->up.t.sockfd);
    c->up_in_poller = 0;
    c->up_watched = c->up_writable_watched = 0;
    http_upstream_release(&c->up, keep);

    /* An idle pooled connection can be closed by the upstream at any moment,
     * and the close is only discoverable by using it: the send appears to
     * succeed into a socket buffer and the read then returns nothing. Zero
     * bytes is not an empty response, it is no response, and the request
     * cannot have been acted on twice from here, so it goes again down a
     * fresh connection. The blocking client does exactly this; without it the
     * empty buffer is parsed as status 0 and the client is sent
     * "HTTP/1.1 0 Unknown". */
    if (got == 0 && was_reused && !c->up_stale_retried) {
        c->up_stale_retried = 1;
        c->dial_fresh = 1;          /* #3260 */
        ev_reclaim_rxbuf(c);
        return ev_begin_retry(d, c);
    }

    /* Answer from the upstream's own bytes when nothing needs the response
     * object. Tried before it is built, because building it is the copy this
     * avoids. The receive buffer stays with the connection until the response
     * has been written, so the body can be sent from where it landed. */
    /* The attempt is over either way below: its outbound request and URL go
     * (#3242). A retry builds its own. */
    int answered = ev_try_direct(d, c);
    if (answered) {
        ev_release_attempt(c);
        return answered;                /* 1 to write it, -1 to close */
    }

    HttpResponse* resp = http_response_alloc_empty();
    if (!resp) return -1;
    if (c->x.buf) http_response_fill_from_bytes(resp, c->x.buf, c->x.len);
    c->px.resp = resp;
    ev_release_attempt(c);

    int r = aether_proxy_exchange_resume(&c->px, 0);
    if (r == PX_NEED_SEND) {
        /* A retry. The blocking driver would send again here; this one has to
         * dial again, which is the same path as the first attempt. */
        return ev_begin_retry(d, c);
    }
    return ev_respond_from(d, c);
}

/* Move a connection as far as it can go right now.
 *
 * Called when a descriptor it owns is ready, and again after each state
 * completes, so a request that can run start to finish without waiting does
 * so in one visit. Returns 0 when the connection is parked on a descriptor,
 * -1 when it is finished with. A parked connection's descriptors then hold
 * what its state waits on, on a backend with no edge mode (#3258).
 */
static int ev_advance_states(EvDriver* d, EvConn* c);
static int ev_advance(EvDriver* d, EvConn* c) {
    int r = ev_advance_states(d, c);
    if (r == 0 && !d->edge_triggered && ev_level_settle(d, c) != 0) return -1;   /* #3278 */
    return r;
}

static int ev_advance_states(EvDriver* d, EvConn* c) {
    for (;;) {
        switch (c->state) {
        case EV_READ_REQUEST: {
            size_t had = c->in_len;
            int r = ev_step_read_request(d, c);
            /* Waiting on the client: no longer than the idle timeout since
             * its last byte (#3243). */
            if (r == 0 && (c->in_len != had || c->idle_due == 0)) ev_arm_idle(d, c);
            if (r <= 0) return r;
            c->idle_due = 0;   /* the request is here */
            r = ev_begin_upstream(d, c);
            if (r <= 0) return r;
            continue;
        }
#ifdef AETHER_HAS_OPENSSL
        case EV_TLS_HANDSHAKE: {
            int r = ev_step_tls_handshake(d, c);
            if (r == 0 && c->idle_due == 0) ev_arm_idle(d, c);   /* #3243 */
            if (r <= 0) return r;
            c->idle_due = 0;
            continue;
        }
#endif
        case EV_UPSTREAM_DIAL: {
            int done = http_upstream_connected(&c->up);
            if (done < 0 && http_upstream_has_next(&c->up)) {
                /* The upstream's name has another address: an IPv6 one that
                 * refused while the server listens on IPv4, say. Dial that
                 * before calling the upstream down. */
                ev_drop_upstream(d, c);
                ev_reclaim_rxbuf(c);
                int r = ev_dial(d, c, 1);
                if (r <= 0) return r;
                continue;
            }
            if (done < 0) {
                int r = ev_upstream_failed(d, c, "connection failed", 1);
                if (r <= 0) return r;
                continue;
            }
            if (done == 0) return 0;   /* woken for something else; keep waiting */
            c->state = EV_UPSTREAM_SEND;
            continue;
        }
        case EV_UPSTREAM_SEND: {
            int r = ev_step_upstream_send(d, c);
            /* The same stale pooled connection as below, found on the write
             * instead of the read: the peer had already gone, and the write
             * is what discovers it. Nothing was delivered, so the request
             * goes again down a fresh connection rather than the client
             * being dropped without an answer. */
            if (r < 0 && c->up.reused && !c->up_stale_retried) {
                c->up_stale_retried = 1;
                c->dial_fresh = 1;          /* #3260 */
                ev_drop_upstream(d, c);
                ev_reclaim_rxbuf(c);
                r = ev_begin_retry(d, c);
                if (r <= 0) return r;
                continue;
            }
            if (r < 0) {
                r = ev_upstream_failed(d, c, "send failed", 1);
                if (r <= 0) return r;
                continue;
            }
            if (r == 0) return 0;
            c->state = EV_UPSTREAM_RECV;
            continue;
        }
        case EV_UPSTREAM_RECV: {
            int r = ev_step_upstream_recv(d, c);
            /* The third way a closed pooled connection announces itself: the
             * peer sent a reset rather than a clean end, so the read fails
             * instead of returning nothing. Same conclusion as the other two,
             * since nothing had arrived: dial fresh and send once more. */
            if (r < 0 && c->x.len == 0 && c->up.reused && !c->up_stale_retried) {
                c->up_stale_retried = 1;
                c->dial_fresh = 1;          /* #3260 */
                ev_drop_upstream(d, c);
                ev_reclaim_rxbuf(c);
                r = ev_begin_retry(d, c);
                if (r <= 0) return r;
                continue;
            }
            if (r < 0) {
                r = ev_upstream_failed(d, c, "connection closed before a response", 1);
                if (r <= 0) return r;
                continue;
            }
            if (r == 0) return 0;
            r = ev_finish_upstream(d, c);
            if (r <= 0) return r;
            continue;
        }
        case EV_BACKOFF:
            return 0;    /* the timer moves it on (ev_expire) */
        case EV_UPSTREAM_LOOKUP:
            return 0;    /* the resolver moves it on (ev_resume_lookups) */
        case EV_CLIENT_SEND: {
            size_t had = c->out_sent + c->out_body_sent;
            int r = ev_step_client_send(d, c);
            /* Waiting on a client that is not reading: no longer than the
             * idle timeout since the last byte it took (#3257). */
            if (r == 0 && (c->out_sent + c->out_body_sent != had || c->idle_due == 0))
                ev_arm_idle(d, c);
            if (r <= 0) return r;
            c->idle_due = 0;   /* written */
            c->requests_served++;
            /* A middleware's answer settled as closing the connection
             * (ev_answer) closes it now that it is written, as the worker
             * path does (#3214). */
            if (c->close_after) return -1;
            /* Done with this request. The connection stays, and the next one
             * is read the same way the first was, so a client that keeps
             * talking never costs another accept. */
            ev_conn_reset_request(d, c);
            c->state = EV_READ_REQUEST;
            continue;
        }
        case EV_CLOSING:
        default:
            return -1;
        }
    }
}

/* ---- the driver loop ----
 *
 * Poll, then move every connection the poller named as far as it can go. The
 * thread sleeps only in the poll, and only when nothing it owns can make
 * progress, which is the whole difference from a worker per connection.
 */

/* The resolver answered `name`, which a connection on some driver may wait
 * on (#3267): runs on the resolver's thread, puts the name in each running
 * driver's list and tells it down its pipe. One note per driver until it
 * reads it: the flag says one is on its way, and a pipe too full to take one
 * wakes the driver anyway. */
static void ev_names_answered(void* ctx, unsigned name) {
    HttpEvLoop* loop = (HttpEvLoop*)ctx;
    for (int i = 0; i < loop->driver_count; i++) {
        EvDriver* d = &loop->drivers[i];
        if (!atomic_load(&d->started)) continue;
        pthread_mutex_lock(&d->answered_lock);
        int cap = (int)(sizeof(d->answered_names) / sizeof(d->answered_names[0]));
        if (d->answered_count < cap) d->answered_names[d->answered_count++] = name;
        else                         d->answered_all = 1;
        pthread_mutex_unlock(&d->answered_lock);
        if (atomic_exchange(&d->answered, 1)) continue;
        int note = EV_NOTE_ANSWERED;
        ssize_t ignored = write(d->wake_w, &note, sizeof(note));
        (void)ignored;
    }
}

/* The connections here waiting on a name the resolver answered dial again
 * (#3267); the rest keep waiting, not sent through the resolver's lock for
 * a name that is not theirs (#3283). A dial whose name has no answer after
 * all (two names sharing a hash) parks again. The list is taken whole first
 * so that none is walked twice. */
static void ev_resume_lookups(EvDriver* d) {
    unsigned names[16];
    pthread_mutex_lock(&d->answered_lock);
    int count = d->answered_count, all = d->answered_all;
    memcpy(names, d->answered_names, (size_t)count * sizeof(names[0]));
    d->answered_count = 0;
    d->answered_all = 0;
    pthread_mutex_unlock(&d->answered_lock);

    EvConn* list = d->lookups;
    d->lookups = NULL;
    while (list) {
        EvConn* c = list;
        list = c->lookup_next;
        c->lookup_prev = c->lookup_next = NULL;
        int answered = all;
        for (int i = 0; i < count && !answered; i++) answered = names[i] == c->lookup_name;
        if (!answered) {
            c->lookup_next = d->lookups;              /* back on the list */
            if (d->lookups) d->lookups->lookup_prev = c;
            d->lookups = c;
            continue;
        }
        c->lookup_waiting = 0;
        int r = ev_dial(d, c, 0);
        if (r > 0) r = ev_advance(d, c);
        else if (r == 0 && !d->edge_triggered && ev_level_settle(d, c) != 0) r = -1;
        if (r < 0) ev_conn_close(d, c);
    }
}

/* A submitted descriptor arrives down a pipe rather than through a lock: the
 * accept path writes, the driver reads, and the driver's own poller is what
 * tells it there is something to read. */
static void ev_take_submissions(EvDriver* d) {
    int fd = -1;
    for (;;) {
        ssize_t n = read(d->wake_r, &fd, sizeof(fd));
        if (n != (ssize_t)sizeof(fd)) break;
        if (fd < 0) continue;

        EvConn* c = (EvConn*)calloc(1, sizeof(EvConn));
        if (!c) { close(fd); continue; }
        c->client_fd = fd;
        c->state = EV_READ_REQUEST;
        c->heap_pos = -1;
        c->up.t.sockfd = -1;
        c->up.t.applied_timeout_ns = -1;
        ev_set_nonblocking(fd);
        ev_set_nodelay(fd);

#ifdef AETHER_HAS_OPENSSL
        /* A TLS server hands this driver an accepted socket with nothing read
         * from it yet, so the handshake is this driver's to carry. It is done
         * without waiting, like everything else here: a connection mid
         * handshake costs a slot, not a thread. */
        if (d->loop->server && d->loop->server->tls_enabled) {
            /* Taken with a reference, so a re-load of the server's TLS
             * cannot free it before SSL_new holds its own (#3371). */
            SSL_CTX* ctx = (SSL_CTX*)http_server_tls_ctx_ref(d->loop->server);
            if (!ctx) { close(fd); free(c); continue; }
            c->ssl = SSL_new(ctx);
            SSL_CTX_free(ctx);
            if (!c->ssl || SSL_set_fd(c->ssl, fd) != 1) {
                if (c->ssl) SSL_free(c->ssl);
                close(fd);
                free(c);
                continue;
            }
            c->state = EV_TLS_HANDSHAKE;
        }
#endif
        atomic_fetch_add(&d->loop->active, 1);
        atomic_fetch_add(&d->loop->taken, 1);

        /* Watched once, for as long as this driver owns it. Every later wait
         * on this descriptor is then free. */
        if (ev_watch(d, fd, c) != 0) { ev_conn_close(d, c); continue; }
        if (ev_advance(d, c) < 0) ev_conn_close(d, c);
    }
    /* The resolver answered (ev_names_answered): the flag is read after the
     * pipe is, so an answer that lands after it was cleared writes a note the
     * next wait sees. */
    if (atomic_exchange(&d->answered, 0)) ev_resume_lookups(d);
    /* One-shot registration: the pipe has to be armed again for the next one. */
    aether_io_poller_add(&d->poller, d->wake_r, NULL, AETHER_IO_READ);
}

static void* ev_driver_main(void* arg) {
    EvDriver* d = (EvDriver*)arg;
    AetherIoEvent events[64];

    /* One of the server's own threads while the run lasts: the middleware
     * ahead of the proxy runs here, and one that stops the server must not
     * wait for the run, which joins this thread. */
    http_server_thread_enter(d->loop->server);
    aether_io_poller_add(&d->poller, d->wake_r, NULL, AETHER_IO_READ);

    while (!atomic_load(&d->loop->stopping)) {
        /* Wait no longer than the next deadline, so a timeout is answered
         * when it is due rather than whenever the next event happens to
         * arrive. With nothing due, the cap is what makes the stop flag
         * visible. */
        int n = aether_io_poller_poll(&d->poller, events, 64,
                                      ev_next_timeout_ms(d, 200));
        if (n > 0)
            atomic_store_explicit(&d->wakeups,
                                  atomic_load_explicit(&d->wakeups, memory_order_relaxed) + 1,
                                  memory_order_relaxed);

        /* Pin the clock for this pass, after the wait and not before: the poll
         * above can block for its whole timeout. Everything below then shares
         * one counter access rather than taking one to arm each deadline,
         * expire each idle pooled connection and record each upstream pick. */
        http_clock_pin();
        for (int i = 0; i < n; i++) {
            int fd = events[i].fd;
            if (fd == d->wake_r) { ev_take_submissions(d); continue; }
            EvConn* c = (fd >= 0 && fd < d->by_fd_cap) ? d->by_fd[fd] : NULL;
            if (!c) continue;
            if (ev_advance(d, c) < 0) ev_conn_close(d, c);
        }

        /* Everything whose deadline has passed, answered rather than dropped. */
        int64_t now = aether_proxy_now_ms();
        while (d->timer_count > 0 && d->timers[0]->deadline_ms <= now) {
            EvConn* c = d->timers[0];
            int r = ev_expire(d, c);
            if (r < 0) { ev_conn_close(d, c); continue; }
            if (ev_advance(d, c) < 0) ev_conn_close(d, c);
        }
        /* Quiet clients past their idle timeout, which the heap no longer
         * carries (#3269). The wait above is capped well under the sweep's
         * period, so it runs on time. */
        ev_sweep_idle(d, now);

        /* Released before waiting again, so the next wait's timeout is
         * computed from the real clock rather than from this pass's. */
        http_clock_unpin();
    }

    /* Shutting down: the connections this driver owns are its to close. */
    for (int fd = 0; fd < d->by_fd_cap; fd++) {
        EvConn* c = d->by_fd[fd];
        if (c && c->client_fd == fd) ev_conn_close(d, c);
    }
    http_server_thread_leave(d->loop->server);
    return NULL;
}

int http_evloop_submit(HttpEvLoop* loop, int client_fd) {
    if (!loop || client_fd < 0 || atomic_load(&loop->stopping)) return -1;
    /* Round-robin. A connection is placed once and then belongs to that
     * driver, so this is the only point where the choice is made. */
    int start = atomic_fetch_add(&loop->next_driver, 1);
    for (int i = 0; i < loop->driver_count; i++) {
        EvDriver* d = &loop->drivers[(start + i) % loop->driver_count];
        if (!atomic_load(&d->started)) continue;
        if (write(d->wake_w, &client_fd, sizeof(client_fd)) == (ssize_t)sizeof(client_fd))
            return 0;
    }
    return -1;
}

/* ---- lifecycle ---- */

static int ev_driver_init(HttpEvLoop* loop, EvDriver* d, int index) {
    memset(d, 0, sizeof(*d));
    d->loop = loop;
    d->index = index;
    d->wake_r = d->wake_w = -1;
    atomic_init(&d->wakeups, 0);
    atomic_init(&d->started, 0);
    atomic_init(&d->answered, 0);
    if (pthread_mutex_init(&d->answered_lock, NULL) != 0) return -1;

    if (aether_io_poller_init(&d->poller) != 0) {
        pthread_mutex_destroy(&d->answered_lock);
        return -1;
    }
    d->edge_triggered = aether_io_poller_edge_capable();

    int pipefd[2];
    /* #3046: close-on-exec, so a child the server starts never holds it. */
    if (aether_pipe_cloexec(pipefd) != 0) {
        aether_io_poller_destroy(&d->poller);
        pthread_mutex_destroy(&d->answered_lock);
        return -1;
    }
    d->wake_r = pipefd[0];
    d->wake_w = pipefd[1];
    /* The driver must never block reading its own pipe, and the accept path
     * must never block writing to it. */
    ev_set_nonblocking(d->wake_r);
    ev_set_nonblocking(d->wake_w);
    return 0;
}

static void ev_driver_destroy(EvDriver* d) {
    if (d->wake_r >= 0) close(d->wake_r);
    if (d->wake_w >= 0) close(d->wake_w);
    aether_io_poller_destroy(&d->poller);
    pthread_mutex_destroy(&d->answered_lock);
    free(d->by_fd);
}

HttpEvLoop* http_evloop_start(HttpServer* server, int threads) {
    if (!server || threads <= 0) return NULL;
    /* No proxy mounted means nothing for this driver to do, and it says so by
     * not existing rather than by running empty. Nor does a server whose
     * proxied requests get something that may block: a middleware ahead of
     * the proxy, or a request hook, not registered as loop-safe
     * (http_server_evloop_serves, #3214). */
    if (!http_server_proxy_opts(server) || !http_server_evloop_serves(server)) return NULL;

    HttpEvLoop* loop = (HttpEvLoop*)calloc(1, sizeof(HttpEvLoop));
    if (!loop) return NULL;
    loop->server = server;
    loop->drivers = (EvDriver*)calloc((size_t)threads, sizeof(EvDriver));
    if (!loop->drivers) { free(loop); return NULL; }
    loop->driver_count = threads;
    atomic_init(&loop->active, 0);
    atomic_init(&loop->taken, 0);
    atomic_init(&loop->stopping, 0);
    atomic_init(&loop->next_driver, 0);
    atomic_init(&loop->body_held, 0);
    loop->body_slots = http_pool_default_workers();
    /* Before the drivers, which ask it from their first dial. Without one a
     * dial would look names up on the driver's thread, so no resolver, no
     * loop (#3267). */
    loop->resolver = http_resolver_new(ev_names_answered, loop);
    if (!loop->resolver) {
        free(loop->drivers);
        free(loop);
        return NULL;
    }

    int started = 0;
    for (int i = 0; i < threads; i++) {
        EvDriver* d = &loop->drivers[i];
        if (ev_driver_init(loop, d, i) != 0) continue;
        if (pthread_create(&d->thread, NULL, ev_driver_main, d) != 0) {
            ev_driver_destroy(d);
            continue;
        }
        atomic_store(&d->started, 1);
        started++;
    }
    if (started == 0) {
        http_resolver_free(loop->resolver);
        free(loop->drivers);
        free(loop);
        return NULL;      /* the caller keeps the worker path */
    }
    return loop;
}

void http_evloop_stop(HttpEvLoop* loop) {
    if (!loop) return;
    atomic_store(&loop->stopping, 1);
    for (int i = 0; i < loop->driver_count; i++) {
        EvDriver* d = &loop->drivers[i];
        if (!atomic_load(&d->started)) continue;
        /* Wake a driver parked in poll so it sees the stop. */
        int sentinel = -1;
        ssize_t ignored = write(d->wake_w, &sentinel, sizeof(sentinel));
        (void)ignored;
    }
    for (int i = 0; i < loop->driver_count; i++) {
        EvDriver* d = &loop->drivers[i];
        if (!atomic_load(&d->started)) continue;
        pthread_join(d->thread, NULL);
    }
    /* After the drivers, which ask it, and before their pipes close, which
     * its threads write to until a lookup under way has ended (#3267). */
    http_resolver_free(loop->resolver);
    for (int i = 0; i < loop->driver_count; i++) {
        EvDriver* d = &loop->drivers[i];
        if (!atomic_load(&d->started)) continue;
        free(d->timers);
        ev_driver_destroy(d);
    }
    free(loop->drivers);
    free(loop);
}

#else /* no threads, no poller, or no pipe: the worker path is unchanged */

HttpEvLoop* http_evloop_start(HttpServer* server, int threads) {
    (void)server; (void)threads;
    return NULL;   /* the caller keeps giving each connection a worker */
}
int  http_evloop_submit(HttpEvLoop* loop, int client_fd) {
    (void)loop; (void)client_fd; return -1;
}
int  http_evloop_active(HttpEvLoop* loop) { (void)loop; return 0; }
int  http_evloop_taken(HttpEvLoop* loop)  { (void)loop; return 0; }
int64_t http_evloop_wakeups(HttpEvLoop* loop) { (void)loop; return 0; }
struct HttpResolver* http_evloop_resolver(HttpEvLoop* loop) { (void)loop; return NULL; }
void http_evloop_stop(HttpEvLoop* loop)   { (void)loop; }

#endif /* AETHER_EVLOOP_SUPPORTED && AETHER_HAS_THREADS */
