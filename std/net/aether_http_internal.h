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

typedef struct {
    size_t header_bytes;
    size_t body_target;
    int    chunked;
    int    definite;
    int    invalid;    /* the response did not say where its body ends */
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
} HttpExchange;

/* An upstream connection acquired without blocking. */
struct HttpUpstreamConnOpaque {
    Transport t;
    int  reused;
    int  connecting;
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
int  http_exchange_send(HttpExchange* x);
int  http_exchange_recv(HttpExchange* x);


/* Split a URL into the pieces a dial needs. Returns 0 when it is not one. */
int parse_url(const char* url, char* host, size_t host_size,
              int* port, char* path, size_t path_size, int* use_tls);

/* Accessors a driver needs from an outbound request it did not build. The
 * request itself stays opaque: these are what it takes to put it on a wire. */
const char* http_request_method_of(const HttpClientRequest* req);
const char* http_request_url_of(const HttpClientRequest* req);
const char* http_request_body_of(const HttpClientRequest* req, int* out_len);
const char* http_request_content_type_of(const HttpClientRequest* req);


/* An empty response object, and filling one in from a complete buffer. */
HttpResponse* http_response_alloc_empty(void);
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

#endif // AETHER_HTTP_INTERNAL_H
