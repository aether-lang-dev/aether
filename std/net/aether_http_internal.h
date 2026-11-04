#ifndef AETHER_HTTP_INTERNAL_H
#define AETHER_HTTP_INTERNAL_H

/* Types shared between the HTTP client and a driver that runs many
 * connections on one thread.
 *
 * These are not part of the public surface: they are here rather than inside
 * aether_http.c because the event driver holds a transport and an exchange of
 * its own, and the alternative was a second definition of each.
 */

#include "aether_http.h"
#include <stdint.h>
#include <stddef.h>

#ifdef AETHER_HAS_OPENSSL
#include <openssl/ssl.h>
#endif

typedef struct {
    int sockfd;
    /* Whether this socket is currently non-blocking: -1 not yet known, 0 no,
     * 1 yes. The two callers want opposite modes and both used to set it on
     * every borrow, which is two fcntl calls each way on a connection whose
     * mode had not changed since the last time. */
    int nonblocking;
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
    /* The receive buffer this connection's last response was read into, kept
     * for the next one (#2160), or NULL.
     *
     * The blocking client path read every response into a buffer that
     * started at nothing and grew to at least 16 KiB, then freed it when the
     * request ended -- so a pooled connection serving thousands of requests
     * allocated and freed 16 KiB each time, even for a 200-byte response.
     * Under #1739's census that was 5.3 MiB over 300 proxied requests, the
     * largest byte figure by an order of magnitude.
     *
     * It lives HERE, rather than beside the pool entry, because a Transport
     * is exactly what the pool stores and returns whole, and exactly what
     * every disposal path already hands to transport_close -- so the buffer
     * rides with the connection into and out of the pool, and is freed on
     * every path that retires one, without a single new ownership rule.
     * Accounted through aether_caps (#461): `rxcap` is the size to free. */
    char*   rxbuf;
    size_t  rxcap;
#ifdef AETHER_HAS_OPENSSL
    SSL* ssl;
    /* A per-request SSL_CTX, owned by this transport, or NULL when the
     * shared process-wide CTX was used. Non-NULL only for the set_cafile
     * pin path (#1107/#1110), which needs its own CTX whose trust store is
     * loaded from the couriered CA. Freed in transport_close AFTER the SSL
     * that references it. */
    SSL_CTX* owned_ctx;
#endif
    /* A pure-Aether TLS 1.3 connection (#1849), or NULL. Set only in a build
     * without OpenSSL, where it is the ONLY way to do https. Deliberately not
     * inside the AETHER_HAS_OPENSSL guard: the field must exist in every build
     * so the transport layout does not change with the TLS backend, and so an
     * OpenSSL build can still fall back if its handshake is unavailable. */
    void* pure_tls;
} Transport;

typedef struct {
    size_t header_bytes;
    size_t body_target;
    int    chunked;
    int    definite;
    int    invalid;    /* the response did not say where its body ends */
    int    bad_head;   /* ... because its header section holds a NUL or a bare CR */
} HttpRespFraming;

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
    /* The whole exchange's deadline, in the http_now_ms clock; 0 = none.
     * `set_timeout` is a per-request timeout, but the socket option it
     * reaches (SO_RCVTIMEO) bounds one recv: a peer that sends a byte just
     * under the limit, forever, kept a "1 s" request open without ever
     * breaking that limit. The recv loop checks this too, so the request
     * ends when the caller's budget does. */
    int64_t deadline_ms;
    int    timed_out;    /* the deadline passed before the response ended */
} HttpExchange;

/* An upstream connection acquired without blocking. */
struct HttpUpstreamConnOpaque {
    Transport t;
    int  reused;
    int  connecting;
    /* The addresses a fresh dial has tried (a hash of each), and how many the
     * name resolved to: http_upstream_redial skips the tried ones by value,
     * not by position, because the list's order can change between dials
     * (another connection may have recorded which family works). */
    unsigned addr_tried[8];
    int  addr_n_tried;
    int  addr_total;
    /* What a fresh dial was for, so the family that connects can be
     * remembered (aether_net_note_family) once the connect completes. Empty
     * for a pooled connection, and for a name with one address. */
    char dial_host[128];
    int  dial_port;
    /* The resolver a dial that may not wait on a name lookup asks
     * (http_upstream_acquire_nb, the proxy event loop's), and why one failed
     * for its name: 1 the answer is still to come, -1 the name does not
     * resolve (#3267). */
    HttpResolver* resolver;
    int  lookup;
    char pool_key[512];
};

#define AE_X_DONE        0
#define AE_X_WANT_READ   1
#define AE_X_WANT_WRITE  2
#define AE_X_ERROR     (-1)


/* The exchange: the only place a client call waits for the peer. A driver
 * that owns its thread loops until DONE and never sees a WANT, because a
 * blocking transport does not produce one. A driver that cannot block arms
 * the descriptor the WANT names and comes back. */
void http_exchange_init(HttpExchange* x, Transport* t,
                        const char* head, size_t head_len,
                        const char* body, int body_len,
                        const char* method);
/* Arm the whole-exchange deadline from a request timeout (ns; 0 = none). */
void http_exchange_set_timeout(HttpExchange* x, int64_t timeout_ns);
int  http_exchange_send(HttpExchange* x);
int  http_exchange_recv(HttpExchange* x);


/* Split a URL into the pieces a dial needs. Returns 0 when it is not one, or
 * when the host or the path does not fit its buffer: a piece is never cut
 * short, since a cut path is still a request, for the wrong thing (#2674).
 * `path` is the request target, the path and the query without the fragment,
 * and may be NULL for a caller that needs the host only. */
int parse_url(const char* url, char* host, size_t host_size,
              int* port, char* path, size_t path_size, int* use_tls);

/* parse_url with the path allocated to fit (never more than the URL and a "/"):
 * *path_out is malloc'd and the caller frees it. Returns 0 on a URL that is
 * not one, a host that does not fit, or no memory, with *path_out NULL. */
int parse_url_alloc(const char* url, char* host, size_t host_size,
                    int* port, char** path_out, int* use_tls);

/* The URI reference `ref` resolved against the absolute URI `base` (RFC 3986
 * 5.2), recomposed with its scheme in lower case and a space or a byte past
 * ASCII in its path, query or fragment percent-encoded. malloc'd; NULL when
 * either is not a URI reference (a reference holding a control byte is
 * not), the base has no scheme, or no memory. */
char* http_uri_resolve(const char* base, const char* ref);

/* The URL a redirect's Location names: http_uri_resolve against the URL that
 * was asked for, read as parse_url reads it (no http:// or https:// is
 * http), the base's fragment kept when the Location has none (RFC 9110
 * 10.2.2). malloc'd, or NULL. */
char* http_resolve_location(const char* base_url, const char* location);

/* A proxy URL, [http://]host[:port], split as a target URL's authority is: an
 * IPv6 host in brackets comes back bare. *port is 3128 when none is named. 0,
 * or -1 for a URL that is not one. */
int http_parse_proxy_url(const char* url, char* host, size_t host_size, int* port);

/* A forward proxy's reply to CONNECT on the socket `fd`, read to the end of
 * its head and no further, so the bytes after it stay for the tunnel's first
 * read. The status, or 0 with *why set when the reply ended or the read
 * failed before its head did, the head is longer than 4096 bytes, or it is
 * malformed (no "HTTP/1." status line with a three-digit status, a NUL or a
 * bare CR). */
int http_connect_reply_read(int fd, const char** why);

/* Accessors a driver needs from an outbound request it did not build. The
 * request itself stays opaque: these are what it takes to put it on a wire. */
const char* http_request_method_of(const HttpClientRequest* req);
const char* http_request_url_of(const HttpClientRequest* req);
const char* http_request_body_of(const HttpClientRequest* req, int* out_len);
const char* http_request_content_type_of(const HttpClientRequest* req);


/* An empty response object, and filling one in from a complete buffer. */
HttpResponse* http_response_alloc_empty(void);
HttpResponse* http_response_alloc_failed(const char* why);
void http_response_fill_from_bytes(HttpResponse* response, char* buf, size_t len);

/* Serialising a request head. Both drivers put the same bytes on the wire. */
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

char* http_build_request_head(const HttpReqHead* p, size_t* out_len, size_t* out_cap);

/* The same builder over a buffer the caller keeps, so a connection serving
 * many requests allocates one head and reuses it. `*buf` is NULL on the first
 * call and the caller's afterwards; release it with aether_caps_free and
 * `*cap` when the connection ends, because that is how it was taken. */
char* http_build_request_head_into(const HttpReqHead* p,
                                   char** buf, size_t* cap, size_t* out_len);


/* A bump allocator for building one outbound request.
 *
 * A proxied request builds a request object and a header node per forwarded
 * header, and frees them all a moment later. Traced with `perf -e
 * page-faults`, http_request_set_header_raw was the largest identifiable
 * source of faults on the path: not because a malloc is slow, but because the
 * churn keeps handing pages back to the kernel and taking them again.
 *
 * The lifetime is exactly one request, so a bump allocator fits it: allocate
 * by moving a pointer, release everything by resetting one offset. A caller
 * that serves many requests keeps the block and resets it each time.
 *
 * Falls back to malloc when a request is not arena-backed, so nothing that
 * builds a request the ordinary way has to change.
 */
typedef struct HttpArena {
    char*  block;
    size_t cap;
    size_t used;
    int    overflowed;   /* an allocation did not fit; that one used malloc */
} HttpArena;

int   http_arena_init(HttpArena* a, size_t cap);
void* http_arena_alloc(HttpArena* a, size_t n);
void  http_arena_reset(HttpArena* a);
void  http_arena_free(HttpArena* a);

/* Bytes still available in the block. A caller that must not have some of its
 * allocations come from the arena and the rest from malloc, because it tracks
 * their origin with a single flag, asks first and takes all or none. */
size_t http_arena_avail(const HttpArena* a);

/* Build this request's headers in `arena`, and do not free them individually.
 * The caller owns the arena and outlives the request. */
void http_request_use_arena(HttpClientRequest* req, HttpArena* arena);

/* Create a request whose struct, method and URL are bump-allocated from
 * `arena`, which the caller resets when the request is done. Falls back to the
 * owning allocations of http_request_raw when there is no arena or it is full,
 * so the caller frees the result the same way either way. */
HttpClientRequest* http_request_raw_arena(const char* method, const char* url,
                                          HttpArena* arena);

/* The proxy event loop's name lookups, which its drivers never wait on
 * (#3267). A resolver keeps the answers it gets, looks a name up once
 * however many dials ask, and runs each lookup on a thread of its own,
 * started when one is first needed. `answered` is called on that thread
 * after a lookup some dial waited on, answered or not, with the name's
 * http_resolver_name; it must not block. POSIX with threads only, where the
 * loop runs: elsewhere _new gives NULL. Freeing waits for a lookup under
 * way, and nothing may ask after. */
struct AetherAddrList;
HttpResolver* http_resolver_new(void (*answered)(void* ctx, unsigned name), void* ctx);
void          http_resolver_free(HttpResolver* r);
/* The hash `answered` names `host`:`port` by. Two names may share one, so a
 * caller that matches on it may wake a dial whose name is still to come,
 * which then waits again. */
unsigned      http_resolver_name(const char* host, int port);
/* `host`:`port` into `out` without waiting: 1 with the addresses (a
 * numeric address, or the kept answer), 0 when the lookup was asked for and
 * `answered` will say so, -1 when the name does not resolve. */
int           http_resolver_answer(HttpResolver* r, const char* host, int port,
                                   struct AetherAddrList* out);
/* For tests: the lookup the resolver's threads run, in place of
 * getaddrinfo's (NULL puts that back). It returns as aether_addr_resolve
 * does: 0, -2 when the name does not exist, -1 for any other failure. */
void          http_resolver_set_lookup(HttpResolver* r,
                                       int (*lookup)(const char* host, int port,
                                                     struct AetherAddrList* out));
/* For tests: the clock the resolver ages its answers by, in milliseconds,
 * in place of the client's (NULL puts that back). */
void          http_resolver_set_clock(HttpResolver* r, int64_t (*now)(void));

#endif // AETHER_HTTP_INTERNAL_H
