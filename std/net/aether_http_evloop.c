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
#include "../http/proxy/aether_proxy_internal.h"
#include "../../runtime/scheduler/aether_io_poller.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/utils/aether_thread.h"

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
    EV_READ_REQUEST,     /* reading a request from the client */
    EV_UPSTREAM_DIAL,    /* a connect this driver started is still in flight */
    EV_UPSTREAM_SEND,    /* writing the request to the upstream */
    EV_UPSTREAM_RECV,    /* reading the response from the upstream */
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
    size_t   in_hdr_end;    /* offset just past the header terminator, once
                             * found. Kept because a request whose body is
                             * still arriving is asked again, and resuming the
                             * search would then look past the terminator and
                             * never find it. 0 means not found yet */

    char*    out;           /* response bytes owed to the client */
    size_t   out_len, out_sent, out_cap;

    HttpUpstreamConn up;
    HttpExchange     x;
    char*            head;       /* serialised upstream request */
    size_t           head_len, head_cap;

    long     deadline_ms;        /* when the upstream call gives up, 0 = none */
    int      up_writable_watched; /* write interest on the upstream, which is
                                   * only wanted while a write is blocked */
    int      up_watched;         /* registered with the poller at all */

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
    EvConn**       by_fd;        /* descriptor -> the connection using it */
    int            by_fd_cap;
    int            wake_r;       /* a submitted descriptor arrives here */
    int            wake_w;
    int            started;

    /* Connections with a deadline, earliest first. A proxy has to give up on
     * an upstream that never answers, and the loop needs to know when the
     * next one is due without walking everything it owns. */
    EvConn**       timers;
    int            timer_count, timer_cap;
} EvDriver;

struct HttpEvLoop {
    HttpServer* server;
    EvDriver*   drivers;
    int         driver_count;
    atomic_int  active;
    atomic_int  stopping;
    atomic_int  next_driver;     /* round-robin placement of new connections */
};

static int ev_timer_set(EvDriver* d, EvConn* c, long deadline_ms);

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

/* Everything one request owned, released before the next one starts.
 *
 * A connection serves many requests, so anything allocated per request has to
 * be freed per request: what leaks here does not leak once, it leaks for as
 * long as the client keeps talking. The head and the response accumulator go
 * through the capability allocator, so they come back through it too, with
 * the size they were charged at. */
static void ev_conn_reset_request(EvConn* c) {
    c->in_len = 0;
    c->in_scanned = 0;
    c->in_hdr_end = 0;

    if (c->head) aether_caps_free(c->head, c->head_cap);
    c->head = NULL;
    c->head_len = c->head_cap = 0;

    /* Keep the accumulator rather than returning it; the next request on this
     * connection reuses it at whatever size it has grown to. */
    if (c->x.buf) {
        if (c->rxbuf) aether_caps_free(c->rxbuf, c->rxcap);
        c->rxbuf = c->x.buf;
        c->rxcap = c->x.cap;
    }
    memset(&c->x, 0, sizeof(c->x));

    /* The buffer stays with the connection and is only grown; a connection
     * serving many requests stops allocating and freeing one per request. */
    c->out_len = c->out_sent = 0;

    /* The request and the response objects stay with the connection. Each
     * one costs a handful of allocations to build, including two fixed-size
     * arrays of header slots that are identical every time, and a connection
     * serves many requests. They are reset in place when the next request
     * arrives rather than freed and built again. */
    memset(&c->px, 0, sizeof(c->px));
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

static void ev_conn_close(EvDriver* d, EvConn* c) {
    if (!c) return;
    if (c->up.t.sockfd >= 0) {
        aether_io_poller_remove(&d->poller, c->up.t.sockfd);
        ev_untrack(d, c->up.t.sockfd);
        http_upstream_release(&c->up, 0);
    }
    if (c->client_fd >= 0) {
        aether_io_poller_remove(&d->poller, c->client_fd);
        ev_untrack(d, c->client_fd);
        close(c->client_fd);
    }
    ev_timer_set(d, c, 0);
    ev_conn_reset_request(c);
    if (c->req) http_request_free(c->req);
    if (c->res) http_server_response_free(c->res);
    if (c->rxbuf) aether_caps_free(c->rxbuf, c->rxcap);
    if (c->arena_ready) http_arena_free(&c->arena);
    free(c->out);
    free(c->in);
    free(c);
    atomic_fetch_sub(&d->loop->active, 1);
}

/* Is a whole request in the buffer yet?
 *
 * Deliberately the same question the worker path asks, and the answer has to
 * match it: a driver that decided differently would accept a request the
 * other rejects. Headers end at a blank line, then exactly the body the
 * framing declares.
 */
static int ev_request_complete(EvConn* c, size_t* out_total) {
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
        char* found = strstr(c->in + from, "\r\n\r\n");
        c->in_scanned = c->in_len;
        if (!found) return 0;
        c->in_hdr_end = (size_t)((found + 4) - c->in);
    }

    size_t header_bytes = c->in_hdr_end;
    char* hdr_end = c->in + header_bytes - 4;
    char cl[64];
    int differing = 0;
    int count = http_find_header_in_block(c->in, hdr_end, "Content-Length",
                                          cl, sizeof(cl), &differing);
    if (count == 0) { *out_total = header_bytes; return 1; }
    if (differing) return -1;
    char* endp = NULL;
    long declared = strtol(cl, &endp, 10);
    if (!endp || *endp != '\0' || endp == cl || declared < 0) return -1;
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

static int ev_timer_set(EvDriver* d, EvConn* c, long deadline_ms) {
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
    long wait = d->timers[0]->deadline_ms - aether_proxy_now_ms();
    if (wait < 0) wait = 0;
    return wait < cap_ms ? (int)wait : cap_ms;
}

/* Forward declarations: the states call each other, and a request that can
 * run start to finish without waiting walks most of them in one visit. */
static void ev_arm_deadline(EvDriver* d, EvConn* c);
static int  ev_expire(EvDriver* d, EvConn* c);
static int  ev_hand_back(EvDriver* d, EvConn* c);
static int  ev_watch_upstream(EvDriver* d, EvConn* c);
static void ev_lend_rxbuf(EvConn* c);
static int  ev_respond_from(EvDriver* d, EvConn* c);
static int  ev_begin_upstream(EvDriver* d, EvConn* c);
static int  ev_begin_retry(EvDriver* d, EvConn* c);
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
static int ev_watch(EvDriver* d, int fd, EvConn* c) {
    if (ev_track(d, fd, c) != 0) return -1;
    return aether_io_poller_add(&d->poller, fd, c,
                                AETHER_IO_READ | AETHER_IO_EDGE);
}

static int ev_watch_writable(EvDriver* d, int fd, EvConn* c) {
    if (ev_track(d, fd, c) != 0) return -1;
    return aether_io_poller_add(&d->poller, fd, c,
                                AETHER_IO_READ | AETHER_IO_WRITE | AETHER_IO_EDGE);
}

/* Back to watching for readable only, now that the write has drained. */
static int ev_unwatch_writable(EvDriver* d, int fd, EvConn* c) {
    return ev_watch(d, fd, c);
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
static int ev_watch_upstream(EvDriver* d, EvConn* c) {
    if (c->up_watched) return 0;
    c->up_watched = 1;
    return ev_watch(d, c->up.t.sockfd, c) == 0 ? 0 : -1;
}

/* Read whatever the client has, without waiting for it. */
static int ev_step_read_request(EvDriver* d, EvConn* c) {
    for (;;) {
        if (ev_in_reserve(c, 4096) != 0) return -1;
        ssize_t n = recv(c->client_fd, c->in + c->in_len,
                         c->in_cap - c->in_len - 1, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            size_t total = 0;
            int complete = ev_request_complete(c, &total);
            if (complete < 0) return -1;         /* framing it cannot resolve */
            if (complete) return 1;              /* a whole request is here */
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
    while (c->out_sent < c->out_len) {
        ssize_t n = send(c->client_fd, c->out + c->out_sent,
                         c->out_len - c->out_sent, 0);
        if (n > 0) { c->out_sent += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return ev_watch_writable(d, c->client_fd, c) == 0 ? 0 : -1;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 1;
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



/* The proxy has produced a response. Turn it into bytes and hand it to the
 * client-writing state, which is the only thing left to wait for. */
static int ev_respond_from(EvDriver* d, EvConn* c) {
    (void)d;
    size_t len = 0;
    if (!http_response_serialize_into(c->res, &c->out, &c->out_cap, &len))
        return -1;
    c->out_len = len;
    c->out_sent = 0;
    c->state = EV_CLIENT_SEND;
    return 1;
}

/* The proxy asked for another attempt, against whichever upstream it picked
 * this time. Same path as the first attempt: a connection, a head, an
 * exchange. */
static int ev_begin_retry(EvDriver* d, EvConn* c) {
    const char* url = http_request_url_of(c->px.outbound);
    char host[256], path[1024];
    int port = 0, use_tls = 0;
    if (!url || !parse_url(url, host, sizeof(host), &port, path, sizeof(path), &use_tls))
        return -1;
    if (use_tls) return -1;

    int fd = http_upstream_acquire(host, port, &c->up);
    if (fd < 0) return -1;

    int body_len = 0;
    const char* body = http_request_body_of(c->px.outbound, &body_len);
    free(c->head);
    HttpReqHead head_params = {
        c->px.outbound, http_request_method_of(c->px.outbound), path,
        host, port, 0, 0, body, body_len,
        http_request_content_type_of(c->px.outbound), 1
    };
    c->head = http_build_request_head(&head_params, &c->head_len, &c->head_cap);
    if (!c->head) return -1;

    memset(&c->x, 0, sizeof(c->x));
    http_exchange_init(&c->x, &c->up.t, c->head, c->head_len, body, body_len,
                       http_request_method_of(c->px.outbound));
    ev_lend_rxbuf(c);
    ev_arm_deadline(d, c);
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



/* Give the connection to the general server path and stop tracking it here.
 *
 * Ownership moves whole: the descriptor leaves this driver's poller and its
 * table before the other path sees it, so no event can arrive for a
 * connection this driver no longer owns.
 */
static int ev_hand_back(EvDriver* d, EvConn* c) {
    int fd = c->client_fd;
    aether_io_poller_remove(&d->poller, fd);
    ev_untrack(d, fd);
    ev_timer_set(d, c, 0);
    c->client_fd = -1;           /* ev_conn_close must not close it now */

    int adopted = http_server_adopt_connection(d->loop->server, fd,
                                               c->in, (int)c->in_len);
    if (adopted != 0) close(fd);
    return -1;                   /* finished with, as far as this driver goes */
}

/* The proxy's configured upstream timeout, as a moment to give up. A proxy
 * that waits forever on an upstream is not one anybody can run: the blocking
 * path gets this from the socket, and a driver that never blocks has to keep
 * the time itself. */
static void ev_arm_deadline(EvDriver* d, EvConn* c) {
    AetherProxyOpts* opts = (AetherProxyOpts*)http_server_proxy_opts(d->loop->server);
    int secs = (opts && opts->pool) ? opts->pool->request_timeout_sec : 0;
    if (secs <= 0) { ev_timer_set(d, c, 0); return; }
    ev_timer_set(d, c, aether_proxy_now_ms() + (long)secs * 1000);
}

/* The upstream did not answer in time. The client is owed an answer anyway,
 * and it is the same one the blocking path gives. */
static int ev_expire(EvDriver* d, EvConn* c) {
    ev_timer_set(d, c, 0);
    if (c->up.t.sockfd >= 0) {
        aether_io_poller_remove(&d->poller, c->up.t.sockfd);
        ev_untrack(d, c->up.t.sockfd);
        http_upstream_release(&c->up, 0);   /* never pool a connection mid-answer */
    }
    if (!c->res) return -1;
    http_response_set_status(c->res, 504);
    http_response_set_header(c->res, "X-Aether-Proxy-Error", "upstream_timeout");
    http_response_set_header(c->res, "Content-Type", "text/plain");
    http_response_set_body(c->res, "upstream timed out\n");
    return ev_respond_from(d, c);
}

/* ---- the bridge to the proxy ----
 *
 * The proxy's own semantics (routing, upstream choice, retries, the breaker,
 * the cache, header rewriting) live in one resumable exchange that the
 * blocking path drives too. This driver supplies the send, which is the only
 * part that differs, so the two cannot drift apart on what a proxied request
 * means.
 */

/* The request is complete: run the proxy up to the point where it needs the
 * upstream, then get a connection and a serialised head ready to go. */
static int ev_begin_upstream(EvDriver* d, EvConn* c) {
    if (!c->req) {
        c->req = (HttpRequest*)calloc(1, sizeof(HttpRequest));
        if (!c->req) return -1;
    }
    if (!http_parse_request_into(c->req, c->in, c->in_len)) return -1;

    if (!c->arena_ready) {
        /* Sized for a request's headers with room to spare. An outlier that
         * does not fit falls back to malloc for the part that overflows. */
        c->arena_ready = http_arena_init(&c->arena, 8192) == 0;
    } else {
        http_arena_reset(&c->arena);
    }

    if (!c->res) {
        c->res = http_response_create();
        if (!c->res) return -1;
    } else {
        http_response_reset(c->res);
    }

    void* opts = http_server_proxy_opts(d->loop->server);
    int r = aether_proxy_exchange_begin(&c->px, c->req, c->res, opts,
                                        c->arena_ready ? &c->arena : NULL);
    if (r == 1) {
        /* Not the proxy's request: a health endpoint, an admin route, or
         * anything another middleware answers. This driver knows one kind of
         * work, so the rest goes back to the path that knows the others,
         * carrying the bytes already read off the socket. */
        return ev_hand_back(d, c);
    }
    if (r != PX_NEED_SEND) {
        /* Served without an upstream: a cache hit or a refusal. The response
         * is already filled in. */
        return ev_respond_from(d, c);   /* 1: the client write is next */
    }

    const char* url = http_request_url_of(c->px.outbound);
    char host[256], path[1024];
    int port = 0, use_tls = 0;
    if (!url || !parse_url(url, host, sizeof(host), &port, path, sizeof(path), &use_tls))
        return -1;
    if (use_tls) return -1;      /* this driver is only mounted on plain HTTP */

    int fd = http_upstream_acquire(host, port, &c->up);
    if (fd < 0) return -1;

    int body_len = 0;
    const char* body = http_request_body_of(c->px.outbound, &body_len);
    HttpReqHead head_params = {
        c->px.outbound, http_request_method_of(c->px.outbound), path,
        host, port, 0, 0, body, body_len,
        http_request_content_type_of(c->px.outbound), 1
    };
    c->head = http_build_request_head(&head_params, &c->head_len, &c->head_cap);
    if (!c->head) return -1;

    http_exchange_init(&c->x, &c->up.t, c->head, c->head_len, body, body_len,
                       http_request_method_of(c->px.outbound));
    ev_lend_rxbuf(c);
    ev_arm_deadline(d, c);
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

/* The upstream has answered. Hand the reply back to the proxy exchange, which
 * decides whether it is the one to keep, and turn what it produced into bytes
 * for the client. */
static int ev_finish_upstream(EvDriver* d, EvConn* c) {
    ev_timer_set(d, c, 0);       /* the upstream answered; nothing to give up on */
    HttpResponse* resp = http_response_alloc_empty();
    if (!resp) return -1;
    if (c->x.buf) http_response_fill_from_bytes(resp, c->x.buf, c->x.len);
    c->px.resp = resp;

    /* The connection is worth keeping only when the response ended where its
     * own framing said it would. */
    int keep = c->x.complete && !c->x.framing.invalid;
    if (c->up_watched) aether_io_poller_remove(&d->poller, c->up.t.sockfd);
    ev_untrack(d, c->up.t.sockfd);
    c->up_watched = c->up_writable_watched = 0;
    http_upstream_release(&c->up, keep);

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
 * -1 when it is finished with.
 */
static int ev_advance(EvDriver* d, EvConn* c) {
    for (;;) {
        switch (c->state) {
        case EV_READ_REQUEST: {
            int r = ev_step_read_request(d, c);
            if (r <= 0) return r;
            r = ev_begin_upstream(d, c);
            if (r <= 0) return r;
            continue;
        }
        case EV_UPSTREAM_DIAL: {
            if (http_upstream_connected(&c->up) != 0) return -1;
            c->state = EV_UPSTREAM_SEND;
            continue;
        }
        case EV_UPSTREAM_SEND: {
            int r = ev_step_upstream_send(d, c);
            if (r <= 0) return r;
            c->state = EV_UPSTREAM_RECV;
            continue;
        }
        case EV_UPSTREAM_RECV: {
            int r = ev_step_upstream_recv(d, c);
            if (r <= 0) return r;
            r = ev_finish_upstream(d, c);
            if (r <= 0) return r;
            continue;
        }
        case EV_CLIENT_SEND: {
            int r = ev_step_client_send(d, c);
            if (r <= 0) return r;
            /* Done with this request. The connection stays, and the next one
             * is read the same way the first was, so a client that keeps
             * talking never costs another accept. */
            ev_conn_reset_request(c);
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
        atomic_fetch_add(&d->loop->active, 1);

        /* Watched once, for as long as this driver owns it. Every later wait
         * on this descriptor is then free. */
        if (ev_watch(d, fd, c) != 0) { ev_conn_close(d, c); continue; }
        if (ev_advance(d, c) < 0) ev_conn_close(d, c);
    }
    /* One-shot registration: the pipe has to be armed again for the next one. */
    aether_io_poller_add(&d->poller, d->wake_r, NULL, AETHER_IO_READ);
}

static void* ev_driver_main(void* arg) {
    EvDriver* d = (EvDriver*)arg;
    AetherIoEvent events[64];

    aether_io_poller_add(&d->poller, d->wake_r, NULL, AETHER_IO_READ);

    while (!atomic_load(&d->loop->stopping)) {
        /* Wait no longer than the next deadline, so a timeout is answered
         * when it is due rather than whenever the next event happens to
         * arrive. With nothing due, the cap is what makes the stop flag
         * visible. */
        int n = aether_io_poller_poll(&d->poller, events, 64,
                                      ev_next_timeout_ms(d, 200));
        for (int i = 0; i < n; i++) {
            int fd = events[i].fd;
            if (fd == d->wake_r) { ev_take_submissions(d); continue; }
            EvConn* c = (fd >= 0 && fd < d->by_fd_cap) ? d->by_fd[fd] : NULL;
            if (!c) continue;
            if (ev_advance(d, c) < 0) ev_conn_close(d, c);
        }

        /* Everything whose deadline has passed, answered rather than dropped. */
        long now = aether_proxy_now_ms();
        while (d->timer_count > 0 && d->timers[0]->deadline_ms <= now) {
            EvConn* c = d->timers[0];
            int r = ev_expire(d, c);
            if (r < 0) { ev_conn_close(d, c); continue; }
            if (ev_advance(d, c) < 0) ev_conn_close(d, c);
        }
    }

    /* Shutting down: the connections this driver owns are its to close. */
    for (int fd = 0; fd < d->by_fd_cap; fd++) {
        EvConn* c = d->by_fd[fd];
        if (c && c->client_fd == fd) ev_conn_close(d, c);
    }
    return NULL;
}

int http_evloop_submit(HttpEvLoop* loop, int client_fd) {
    if (!loop || client_fd < 0 || atomic_load(&loop->stopping)) return -1;
    /* Round-robin. A connection is placed once and then belongs to that
     * driver, so this is the only point where the choice is made. */
    int start = atomic_fetch_add(&loop->next_driver, 1);
    for (int i = 0; i < loop->driver_count; i++) {
        EvDriver* d = &loop->drivers[(start + i) % loop->driver_count];
        if (!d->started) continue;
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

    if (aether_io_poller_init(&d->poller) != 0) return -1;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        aether_io_poller_destroy(&d->poller);
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
    free(d->by_fd);
}

HttpEvLoop* http_evloop_start(HttpServer* server, int threads) {
    if (!server || threads <= 0) return NULL;
    /* No proxy mounted means nothing for this driver to do, and it says so by
     * not existing rather than by running empty. */
    if (!http_server_proxy_opts(server)) return NULL;

    HttpEvLoop* loop = (HttpEvLoop*)calloc(1, sizeof(HttpEvLoop));
    if (!loop) return NULL;
    loop->server = server;
    loop->drivers = (EvDriver*)calloc((size_t)threads, sizeof(EvDriver));
    if (!loop->drivers) { free(loop); return NULL; }
    loop->driver_count = threads;
    atomic_init(&loop->active, 0);
    atomic_init(&loop->stopping, 0);
    atomic_init(&loop->next_driver, 0);

    int started = 0;
    for (int i = 0; i < threads; i++) {
        EvDriver* d = &loop->drivers[i];
        if (ev_driver_init(loop, d, i) != 0) continue;
        if (pthread_create(&d->thread, NULL, ev_driver_main, d) != 0) {
            ev_driver_destroy(d);
            continue;
        }
        d->started = 1;
        started++;
    }
    if (started == 0) {
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
        if (!d->started) continue;
        /* Wake a driver parked in poll so it sees the stop. */
        int sentinel = -1;
        ssize_t ignored = write(d->wake_w, &sentinel, sizeof(sentinel));
        (void)ignored;
    }
    for (int i = 0; i < loop->driver_count; i++) {
        EvDriver* d = &loop->drivers[i];
        if (!d->started) continue;
        pthread_join(d->thread, NULL);
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
void http_evloop_stop(HttpEvLoop* loop)   { (void)loop; }

#endif /* AETHER_EVLOOP_SUPPORTED && AETHER_HAS_THREADS */
