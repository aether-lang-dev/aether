/* The slow and abusive client harness (#3737, #3750).
 *
 * The per-read idle timeout bounds each wait for a byte, so a client that
 * sends one byte just inside it, over and over, held a worker for as long as
 * it liked: through the TLS handshake, a request head, or a request body.
 * Each round here starts a server whose limits are short (idle 600 ms, the
 * handshake, head and body budgets 1 s, a body rate of 1000 bytes a second)
 * and drives one client that sends a byte every 150 ms, well inside the idle
 * timeout. The server must answer (408) or close within LIMIT_MS of the
 * round's start; the old code kept the connection until the client stopped
 * sending (TRICKLE_MS) and then went quiet for the idle timeout.
 *
 * Rounds, each through the backends that apply (plain; TLS through OpenSSL
 * and through the pure-Aether server when the script has a certificate):
 *
 *   handshake   a TLS record header, then its body a byte at a time
 *   head        a request head a byte at a time (over TLS: the record that
 *               carries a whole head, a byte at a time)
 *   h2 probe    the same on a server with HTTP/2 on, where the first 24
 *               bytes were read before the head's clock started
 *   body        a 100-byte body the server reads before the handler runs
 *   stream      a 100000-byte body the handler reads with request_body
 *   read        the same read with request_body_read
 *   chunked     a chunked body
 *   stall       20000 bytes of a 100000-byte body, then nothing: past the
 *               idle timeout the handler had the 20000 and answered 200
 *
 * and two that must succeed:
 *
 *   rate        12000 bytes at 4000 a second: past the 1 s budget, but
 *               above the minimum rate, which extends it (200, len=12000)
 *   sleep       a handler that sleeps 1.5 s between two reads of a body
 *               that has all arrived: the server's own time does not count
 *               against the client (200, read=40000)
 *
 * On Linux and macOS the same against the proxy event loop, a gateway that
 * proxies to an upstream (#3737, #3750): a slow head, a slow body, a slow
 * handshake, a body above the minimum rate (200), and its connection bound:
 * four connections partway through a head and a fifth refused 503, four
 * silent ones and a fifth served while a silent one is closed.
 *
 * And where the descriptor limit binds sockets: with the process out of
 * descriptors and a connection queued, the server must not spin on a
 * listener that stays readable (under 500 ms of CPU in 2 s).
 *
 * The review rounds (#3737, #3738, #3750):
 *
 *   burst       100000 bytes of a body at once, then a byte a tick: the
 *               burst's credit is held to the 1 s rate window, so 408 (on
 *               HTTP/2 the stream reset, on the loop 408) within the
 *               budget, where the whole credit held it for 100 s
 *   h2 probe short request
 *               an 18-byte HTTP/1.0 request on a server with HTTP/2 on is
 *               answered at once, not after the head's deadline
 *   h2 stop     a stop with a stream its client keeps trickling returns at
 *               once, the stream refused (REFUSED_STREAM)
 *   h2 slow handler
 *               a handler past the idle timeout answering 1 MiB to a client
 *               with a 64 KiB window: all of it arrives
 *   key update  a TLS 1.3 KeyUpdate before the first request does not start
 *               the head's clock (both TLS backends)
 *   per ip linger
 *               a connection refused for its address lingers 500 ms, not 2 s
 *   loop endless upstream
 *               the loop holds an upstream's answer to the proxy's cap:
 *               502 response_too_large at once, not 504 after the timeout
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "aether_http_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET client_sock;
#define CLIENT_BAD INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int client_sock;
#define CLIENT_BAD (-1)
#endif

int http_server_port(HttpServer* server);

/* tls_client.c or tls_client_none.c */
int slow_tls_available(void);
void* slow_tls_connect(int fd);
int slow_tls_write(void* t, const void* buf, int len);
int slow_tls_record(void* t, const void* buf, int len, unsigned char* out, int cap);
int slow_tls_read(void* t, char* buf, int cap);
void slow_tls_free(void* t);
int slow_tls_key_update(void* t);
void slow_tls_explain(void);

#define IDLE_MS        600
#define BUDGET_MS      1000
#define MIN_RATE       1000
#define TICK_MS        150    /* well inside IDLE_MS */
#define TRICKLE_MS     4000   /* how long a slow client keeps sending */
#define LIMIT_MS       2400   /* the budget and a margin; the old code took TRICKLE_MS + IDLE_MS */
#define CLIENT_WAIT_MS 5000

enum { PLAIN = 0, OPENSSL = 1, AETHER = 2 };
static const char* backend_name[] = { "plain", "openssl", "aether" };
static const char* g_cert;
static const char* g_key;
static int g_fail;
static int g_pass;

static int64_t now_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static void client_close(client_sock s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static client_sock client_connect(int port) {
    client_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == CLIENT_BAD) return CLIENT_BAD;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
#ifdef _WIN32
    DWORD tv = CLIENT_WAIT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv = { CLIENT_WAIT_MS / 1000, (CLIENT_WAIT_MS % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        client_close(s);
        return CLIENT_BAD;
    }
    return s;
}

/* 1 when `s` is readable (data, or the server's close) within `ms`. */
static int readable(client_sock s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select((int)s + 1, &rf, NULL, NULL, &tv) > 0;
}

static int send_all(client_sock s, const void* p, int len) {
    const char* c = (const char*)p;
    while (len > 0) {
        int n = (int)send(s, c, len, 0);
        if (n <= 0) return -1;
        c += n;
        len -= n;
    }
    return 0;
}

/* The status of the response on `s` (through `tls` when set), 0 when the
 * server closed without one, -1 when nothing came. */
static int read_status(client_sock s, void* tls, char* body, int body_cap) {
    char buf[2048];
    int have = 0;
    while (have < (int)sizeof(buf) - 1) {
        int n = tls ? slow_tls_read(tls, buf + have, (int)sizeof(buf) - 1 - have)
                    : (int)recv(s, buf + have, (int)sizeof(buf) - 1 - have, 0);
        if (n <= 0) break;
        have += n;
        buf[have] = '\0';
        char* end = strstr(buf, "\r\n\r\n");
        if (end && strstr(buf, "Content-Length: ")) {
            int cl = atoi(strstr(buf, "Content-Length: ") + 16);
            if (have - (int)(end + 4 - buf) >= cl) break;
        }
    }
    if (have == 0) {
        /* Over TLS, say how: the macOS head rounds read nothing after the
         * server's cut, and status 0 alone does not tell a close from a
         * reset. */
        if (tls) slow_tls_explain();
        return 0;
    }
    buf[have] = '\0';
    if (body && body_cap > 0) {
        char* end = strstr(buf, "\r\n\r\n");
        snprintf(body, (size_t)body_cap, "%s", end ? end + 4 : "");
    }
    if (strncmp(buf, "HTTP/1.1 ", 9) != 0) return -1;
    return atoi(buf + 9);
}

/* Send `len` bytes of `p` a byte every TICK_MS, for no longer than
 * TRICKLE_MS, stopping once the server answers or closes; then wait for it.
 * Returns the ms from `t0` until `s` was readable, or -1 when it never was. */
static int64_t trickle(client_sock s, const void* p, int len, int64_t t0) {
    const char* c = (const char*)p;
    for (int i = 0; i < len && now_ms() - t0 < TRICKLE_MS; i++) {
        if (readable(s, TICK_MS)) return now_ms() - t0;
        if (send(s, c + i, 1, 0) != 1) return now_ms() - t0;   /* reset: closed */
    }
    if (readable(s, CLIENT_WAIT_MS)) return now_ms() - t0;
    return -1;
}

/* The same over TLS, a one-byte record every tick. */
static int64_t trickle_records(client_sock s, void* tls, const void* p, int len, int64_t t0) {
    const char* c = (const char*)p;
    for (int i = 0; i < len && now_ms() - t0 < TRICKLE_MS; i++) {
        if (readable(s, TICK_MS)) return now_ms() - t0;
        if (slow_tls_write(tls, c + i, 1) != 1) return now_ms() - t0;
    }
    if (readable(s, CLIENT_WAIT_MS)) return now_ms() - t0;
    return -1;
}

static void ping_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    http_response_set_status(res, 200);
    http_response_set_body(res, "pong");
}

/* The body whole, through request_body. */
static void len_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)ud;
    (void)http_request_body(req);
    char out[64];
    snprintf(out, sizeof(out), "len=%lld", (long long)http_request_body_length(req));
    http_response_set_status(res, 200);
    http_response_set_body(res, out);
}

/* The body through request_body_read, sleeping `ud` ms after the first read. */
static void read_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    int pause = (int)(intptr_t)ud;
    int64_t total = 0;
    int reads = 0;
    for (;;) {
        if (!http_request_body_read_raw(req, total, 16384)) break;
        int n = http_get_request_body_read_length();
        http_release_request_body_read();
        if (n <= 0) break;
        total += n;
        if (++reads == 1 && pause > 0) sleep_ms(pause);
    }
    char out[64];
    snprintf(out, sizeof(out), "read=%lld", (long long)total);
    http_response_set_status(res, 200);
    http_response_set_body(res, out);
}

/* A server with the round's limits, started; NULL when the backend is not
 * available (it says why). */
static HttpServer* start_server(int backend, int h2, const char* label) {
    HttpServer* server = http_server_create(0);
    if (!server) { printf("FAIL: [%s] server_create\n", label); g_fail++; return NULL; }
    http_server_get(server, "/ping", ping_handler, NULL);
    http_server_post(server, "/len", len_handler, NULL);
    http_server_post(server, "/read", read_handler, NULL);
    http_server_post(server, "/sleep", read_handler, (void*)(intptr_t)1500);
    const char* err = http_server_set_keepalive_raw(server, 1, 0, (int64_t)IDLE_MS * 1000000LL);
    if (!*err) err = http_server_set_head_timeout_raw(server, (int64_t)BUDGET_MS * 1000000LL);
    if (!*err) err = http_server_set_body_timeout_raw(server, (int64_t)BUDGET_MS * 1000000LL, MIN_RATE);
    if (!*err) err = http_server_set_tls_handshake_timeout_raw(server, (int64_t)BUDGET_MS * 1000000LL);
    if (*err) {
        printf("FAIL: [%s] a limit setter refused: %s\n", label, err);
        g_fail++;
        http_server_free(server);
        return NULL;
    }
    if (h2) {
        err = http_server_set_h2_raw(server, 0);
        if (*err) {
            printf("round[%s]: skipped, %s\n", label, err);
            http_server_free(server);
            return NULL;
        }
    }
    if (backend != PLAIN) {
        if (!g_cert) {
            printf("round[%s]: skipped, no certificate\n", label);
            http_server_free(server);
            return NULL;
        }
        err = http_server_set_tls_raw(server, g_cert, g_key);
        if (!*err) err = http_server_set_tls_backend_raw(server, backend_name[backend]);
        if (*err) {
            printf("round[%s]: skipped, %s\n", label, err);
            http_server_free(server);
            return NULL;
        }
    }
    if (http_server_bind_raw(server, "127.0.0.1", 0) < 0 ||
        http_server_start_background_raw(server) < 0) {
        printf("FAIL: [%s] bind or start: %s\n", label, http_server_error(server));
        g_fail++;
        http_server_free(server);
        return NULL;
    }
    return server;
}

static void report(const char* label, int ok, int64_t elapsed, int status, const char* note) {
    printf("round[%s]: elapsed_ms=%lld status=%d%s%s %s\n", label, (long long)elapsed, status,
           note ? " " : "", note ? note : "", ok ? "passed" : "FAILED");
    if (ok) g_pass++; else g_fail++;
}

/* A client too slow for a limit: answered with `want` (408), or closed when
 * `want` is 0, within LIMIT_MS. */
static void expect_cut(const char* label, int64_t elapsed, int status, int want) {
    int ok = elapsed >= 0 && elapsed < LIMIT_MS && (want ? status == want : status <= 0);
    report(label, ok, elapsed, status, NULL);
}

static void round_handshake(int backend) {
    char label[64];
    snprintf(label, sizeof(label), "handshake %s", backend_name[backend]);
    HttpServer* server = start_server(backend, 0, label);
    if (!server) return;
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    /* A handshake record that declares 512 bytes, then the bytes, slowly. */
    unsigned char rec[5 + 512];
    memset(rec, 0, sizeof(rec));
    rec[0] = 0x16; rec[1] = 0x03; rec[2] = 0x01; rec[3] = 0x02; rec[4] = 0x00;
    int64_t t0 = now_ms();
    send_all(s, rec, 5);
    int64_t elapsed = trickle(s, rec + 5, 512, t0);
    int status = elapsed >= 0 ? read_status(s, NULL, NULL, 0) : -1;
    client_close(s);
    http_server_free(server);
    expect_cut(label, elapsed, status, 0);
}

static void round_head(int backend, int h2) {
    char label[64];
    snprintf(label, sizeof(label), "%s %s", h2 ? "h2 probe" : "head", backend_name[backend]);
    HttpServer* server = start_server(backend, h2, label);
    if (!server) return;
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    void* tls = NULL;
    int64_t elapsed;
    if (backend == PLAIN) {
        static const char head[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: "
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
        int64_t t0 = now_ms();
        elapsed = trickle(s, head, (int)sizeof(head) - 1, t0);
    } else {
        tls = slow_tls_connect((int)s);
        if (!tls) {
            printf("round[%s]: skipped, %s\n", label,
                   slow_tls_available() ? "the TLS handshake failed" : "no TLS client in this build");
            if (slow_tls_available()) g_fail++;
            client_close(s);
            http_server_free(server);
            return;
        }
        /* The record carrying a whole head, a byte at a time: the server
         * cannot see any of the head until the record is whole. */
        static const char head[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        unsigned char rec[512];
        int n = slow_tls_record(tls, head, (int)sizeof(head) - 1, rec, (int)sizeof(rec));
        int64_t t0 = now_ms();
        elapsed = n > 0 ? trickle(s, rec, n, t0) : -1;
    }
    int status = elapsed >= 0 ? read_status(s, tls, NULL, 0) : -1;
    slow_tls_free(tls);
    client_close(s);
    http_server_free(server);
    expect_cut(label, elapsed, status, 408);
}

/* A body sent slowly, after a head sent whole: `kind` "body" (100 bytes the
 * server reads before the handler), "stream" (request_body on a streamed
 * body), "read" (request_body_read), "chunked". */
static void round_body(int backend, const char* kind) {
    char label[64];
    snprintf(label, sizeof(label), "%s %s", kind, backend_name[backend]);
    HttpServer* server = start_server(backend, 0, label);
    if (!server) return;
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    void* tls = NULL;
    if (backend != PLAIN) {
        tls = slow_tls_connect((int)s);
        if (!tls) {
            printf("round[%s]: skipped, %s\n", label,
                   slow_tls_available() ? "the TLS handshake failed" : "no TLS client in this build");
            if (slow_tls_available()) g_fail++;
            client_close(s);
            http_server_free(server);
            return;
        }
    }
    char head[256];
    if (strcmp(kind, "body") == 0) {
        snprintf(head, sizeof(head), "POST /len HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100\r\n\r\n");
    } else if (strcmp(kind, "chunked") == 0) {
        snprintf(head, sizeof(head), "POST /len HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                     "Transfer-Encoding: chunked\r\n\r\n64\r\n");
    } else {
        /* Past the connection buffer, so the handler reads it off the
         * socket. (A fast start would earn the slow rest the minimum
         * rate's credit: the rate round covers that.) */
        snprintf(head, sizeof(head), "POST /%s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100000\r\n\r\n",
                 strcmp(kind, "read") == 0 ? "read" : "len");
    }
    static char slow[512];
    memset(slow, 'y', sizeof(slow));
    int64_t t0 = now_ms();
    int64_t elapsed;
    if (tls) {
        slow_tls_write(tls, head, (int)strlen(head));
        elapsed = trickle_records(s, tls, slow, (int)sizeof(slow), t0);
    } else if (strcmp(kind, "stall") == 0) {
        /* Part of a streamed body, then nothing: the idle timeout ends the
         * wait, and the handler must not answer a body cut short as if it
         * were whole (it answered 200 with len=20000). */
        static char part[20000];
        memset(part, 'x', sizeof(part));
        send_all(s, head, (int)strlen(head));
        send_all(s, part, (int)sizeof(part));
        elapsed = readable(s, CLIENT_WAIT_MS) ? now_ms() - t0 : -1;
    } else {
        send_all(s, head, (int)strlen(head));
        elapsed = trickle(s, slow, (int)sizeof(slow), t0);
    }
    int status = elapsed >= 0 ? read_status(s, tls, NULL, 0) : -1;
    slow_tls_free(tls);
    client_close(s);
    http_server_free(server);
    expect_cut(label, elapsed, status, 408);
}

/* 12000 bytes at 4000 a second: three times the 1 s budget, but above the
 * minimum rate, which extends it. */
static void round_rate(int backend) {
    char label[64];
    snprintf(label, sizeof(label), "rate %s", backend_name[backend]);
    HttpServer* server = start_server(backend, 0, label);
    if (!server) return;
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    void* tls = NULL;
    if (backend != PLAIN && !(tls = slow_tls_connect((int)s))) {
        printf("round[%s]: skipped, no TLS client\n", label);
        if (slow_tls_available()) g_fail++;
        client_close(s);
        http_server_free(server);
        return;
    }
    static const char head[] = "POST /len HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 12000\r\n\r\n";
    static char chunk[1000];
    memset(chunk, 'z', sizeof(chunk));
    int64_t t0 = now_ms();
    if (tls) slow_tls_write(tls, head, (int)sizeof(head) - 1);
    else send_all(s, head, (int)sizeof(head) - 1);
    for (int i = 0; i < 12; i++) {
        sleep_ms(250);
        if (tls) slow_tls_write(tls, chunk, (int)sizeof(chunk));
        else send_all(s, chunk, (int)sizeof(chunk));
    }
    char body[128] = "";
    int status = read_status(s, tls, body, (int)sizeof(body));
    int64_t elapsed = now_ms() - t0;
    slow_tls_free(tls);
    client_close(s);
    http_server_free(server);
    report(label, status == 200 && strcmp(body, "len=12000") == 0, elapsed, status, body);
}

/* A handler that sleeps 1.5 s between two reads of a body that has all
 * arrived: the time is the server's, not the client's. */
static void round_sleep(void) {
    const char* label = "sleep plain";
    HttpServer* server = start_server(PLAIN, 0, label);
    if (!server) return;
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    static const char head[] = "POST /sleep HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 40000\r\n\r\n";
    static char body_bytes[40000];
    memset(body_bytes, 'w', sizeof(body_bytes));
    int64_t t0 = now_ms();
    send_all(s, head, (int)sizeof(head) - 1);
    send_all(s, body_bytes, (int)sizeof(body_bytes));
    char body[128] = "";
    int status = read_status(s, NULL, body, (int)sizeof(body));
    int64_t elapsed = now_ms() - t0;
    client_close(s);
    http_server_free(server);
    report(label, status == 200 && strcmp(body, "read=40000") == 0, elapsed, status, body);
}

#ifndef _WIN32
/* ---- the proxy event loop (Linux and macOS) ----
 *
 * A server whose only work is a reverse proxy to plain upstreams is served
 * by the event loop, not a worker per connection. Its idle timer moved with
 * every byte, and it had no head or body deadline and no bound on the
 * connections it held, so a client sending a byte just inside the idle
 * timeout kept a slot for ever and enough of them used up the process's
 * descriptors. Its gateways here have a 5 s idle timeout, so only the
 * budgets (1 s) can end a slow client within LIMIT_MS. */

void* aether_proxy_pool_new(int algo, int request_timeout_sec, int dial_timeout_ms,
                            int max_inflight_per_up);
const char* aether_proxy_upstream_add(void* pool, const char* base_url, int weight);
void* aether_proxy_opts_new(void);
const char* aether_proxy_use_reverse_proxy(HttpServer* server, const char* path_prefix,
                                           void* pool, void* opts);
void aether_proxy_opts_free(void* opts);
const char* aether_proxy_opts_set_body_cap(void* opts, int max_body_bytes);
void aether_proxy_pool_free(void* pool);
int http_server_evloop_connections(HttpServer* server);

static HttpServer* g_upstream;
static int g_loop_per_ip;   /* a per-address limit for the next gateway */
static int g_gw_upstream_port;   /* the next gateway's upstream, 0 for g_upstream */
static int g_gw_body_cap;        /* the next gateway's proxy body cap, 0 for its default */
static int g_gw_rate_window;     /* the next gateway's body rate window in ms, 0 for its default */
static int g_upstream_port;

/* The upstream every gateway proxies to: a worker-path server. */
static int loop_upstream(void) {
    if (g_upstream) return 1;
    g_upstream = http_server_create(0);
    if (!g_upstream) return 0;
    http_server_get(g_upstream, "/ping", ping_handler, NULL);
    http_server_post(g_upstream, "/len", len_handler, NULL);
    if (http_server_bind_raw(g_upstream, "127.0.0.1", 0) < 0 ||
        http_server_start_background_raw(g_upstream) < 0) return 0;
    g_upstream_port = http_server_port(g_upstream);
    return 1;
}

typedef struct { HttpServer* server; void* pool; void* opts; } Gateway;

/* A gateway on the event loop: the round's budgets, a 5 s idle timeout,
 * at most `max_conns` connections on the loop (0 for the default), TLS
 * through OpenSSL when `tls`. server is NULL when it cannot run. */
static Gateway loop_gateway(const char* label, int tls, int max_conns) {
    Gateway g = { NULL, NULL, NULL };
    if (!loop_upstream()) { printf("FAIL: [%s] upstream\n", label); g_fail++; return g; }
    HttpServer* s = http_server_create(0);
    if (!s) { printf("FAIL: [%s] server_create\n", label); g_fail++; return g; }
    const char* err = http_server_set_keepalive_raw(s, 1, 0, 5000LL * 1000000LL);
    if (!*err) err = http_server_set_head_timeout_raw(s, (int64_t)BUDGET_MS * 1000000LL);
    if (!*err) err = http_server_set_body_timeout_raw(s, (int64_t)BUDGET_MS * 1000000LL, MIN_RATE);
    if (!*err) err = http_server_set_tls_handshake_timeout_raw(s, (int64_t)BUDGET_MS * 1000000LL);
    if (!*err && max_conns) err = http_server_set_loop_max_connections_raw(s, max_conns);
    if (!*err && g_loop_per_ip) err = http_server_set_max_connections_per_ip_raw(s, g_loop_per_ip, 64);
    if (!*err && g_gw_rate_window)
        err = http_server_set_body_rate_window_raw(s, (int64_t)g_gw_rate_window * 1000000LL);
    if (*err) {
        printf("FAIL: [%s] a limit setter refused: %s\n", label, err);
        g_fail++;
        http_server_free(s);
        return g;
    }
    if (tls) {
        if (!g_cert) { printf("round[%s]: skipped, no certificate\n", label); http_server_free(s); return g; }
        err = http_server_set_tls_raw(s, g_cert, g_key);
        if (!*err) err = http_server_set_tls_backend_raw(s, "openssl");
        if (*err) { printf("round[%s]: skipped, %s\n", label, err); http_server_free(s); return g; }
    }
    void* pool = aether_proxy_pool_new(0 /* round robin */, 10, 0, 0);
    char up[64];
    snprintf(up, sizeof(up), "http://127.0.0.1:%d", g_gw_upstream_port ? g_gw_upstream_port : g_upstream_port);
    void* opts = aether_proxy_opts_new();
    if (opts && g_gw_body_cap) aether_proxy_opts_set_body_cap(opts, g_gw_body_cap);
    if (!pool || !opts || *aether_proxy_upstream_add(pool, up, 1) ||
        *aether_proxy_use_reverse_proxy(s, "/", pool, opts) ||
        http_server_bind_raw(s, "127.0.0.1", 0) < 0 ||
        http_server_start_background_raw(s) < 0) {
        printf("FAIL: [%s] gateway setup: %s\n", label, http_server_error(s));
        g_fail++;
        http_server_free(s);
        return g;
    }
    g.server = s;
    g.pool = pool;
    g.opts = opts;
    return g;
}

static void loop_gateway_free(Gateway g) {
    if (!g.server) return;
    http_server_free(g.server);
    aether_proxy_opts_free(g.opts);
    aether_proxy_pool_free(g.pool);
}

/* The round ran on the loop, or says why not. */
static int on_loop(Gateway g, const char* label) {
    if (http_server_evloop_connections(g.server) > 0) return 1;
    printf("round[%s]: the gateway was not served by the event loop\n", label);
    g_fail++;
    return 0;
}

/* A slow head, a slow body read by the loop, and a slow handshake, on the
 * loop; answered 408 (closed for the handshake) within LIMIT_MS. */
static void round_loop_slow(const char* kind) {
    char label[64];
    snprintf(label, sizeof(label), "loop %s", kind);
    int tls = strcmp(kind, "handshake") == 0;
    Gateway g = loop_gateway(label, tls, 0);
    if (!g.server) return;
    client_sock s = client_connect(http_server_port(g.server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); loop_gateway_free(g); return; }
    int64_t t0 = now_ms();
    int64_t elapsed;
    int want = 408;
    if (tls) {
        unsigned char rec[5 + 512];
        memset(rec, 0, sizeof(rec));
        rec[0] = 0x16; rec[1] = 0x03; rec[2] = 0x01; rec[3] = 0x02; rec[4] = 0x00;
        send_all(s, rec, 5);
        elapsed = trickle(s, rec + 5, 512, t0);
        want = 0;
    } else if (strcmp(kind, "head") == 0) {
        static const char head[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: "
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
        elapsed = trickle(s, head, (int)sizeof(head) - 1, t0);
    } else {
        static const char head[] = "POST /len HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100\r\n\r\n";
        static char slow[100];
        memset(slow, 'y', sizeof(slow));
        send_all(s, head, (int)sizeof(head) - 1);
        elapsed = trickle(s, slow, (int)sizeof(slow), t0);
    }
    int status = elapsed >= 0 ? read_status(s, NULL, NULL, 0) : -1;
    client_close(s);
    int ran = on_loop(g, label);
    loop_gateway_free(g);
    if (ran) expect_cut(label, elapsed, status, want);
}

/* 12000 bytes at 4000 a second through the loop: past the 1 s budget, but
 * above the minimum rate. */
static void round_loop_rate(void) {
    const char* label = "loop rate";
    Gateway g = loop_gateway(label, 0, 0);
    if (!g.server) return;
    client_sock s = client_connect(http_server_port(g.server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); loop_gateway_free(g); return; }
    static const char head[] = "POST /len HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 12000\r\n\r\n";
    static char chunk[1000];
    memset(chunk, 'z', sizeof(chunk));
    int64_t t0 = now_ms();
    send_all(s, head, (int)sizeof(head) - 1);
    for (int i = 0; i < 12; i++) {
        sleep_ms(250);
        send_all(s, chunk, (int)sizeof(chunk));
    }
    char body[128] = "";
    int status = read_status(s, NULL, body, (int)sizeof(body));
    int64_t elapsed = now_ms() - t0;
    client_close(s);
    int ran = on_loop(g, label);
    loop_gateway_free(g);
    if (ran) report(label, status == 200 && strcmp(body, "len=12000") == 0, elapsed, status, body);
}

/* The loop's connection bound, 4 here. Four connections partway through a
 * head hold their slots, and a fifth is refused at once with 503. Four that
 * have sent nothing yield: a fifth with a request is served, and the oldest
 * silent one is closed to make room. */
static void round_loop_slots(void) {
    const char* label = "loop slots";
    Gateway g = loop_gateway(label, 0, 4);
    if (!g.server) return;
    int port = http_server_port(g.server);
    client_sock held[4];
    static const char part[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: a";
    for (int i = 0; i < 4; i++) {
        held[i] = client_connect(port);
        if (held[i] != CLIENT_BAD) send_all(held[i], part, (int)sizeof(part) - 1);
    }
    sleep_ms(200);
    static const char req[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    client_sock s = client_connect(port);
    int64_t t0 = now_ms();
    if (s != CLIENT_BAD) send_all(s, req, (int)sizeof(req) - 1);
    int status = s != CLIENT_BAD && readable(s, 3000) ? read_status(s, NULL, NULL, 0) : -1;
    int64_t elapsed = now_ms() - t0;
    if (s != CLIENT_BAD) client_close(s);
    for (int i = 0; i < 4; i++) if (held[i] != CLIENT_BAD) client_close(held[i]);
    int ran = on_loop(g, label);
    if (ran) report("loop slots full of heads", status == 503 && elapsed < 1000, elapsed, status,
                    "a fifth is refused 503");

    /* Four silent connections, then a request. */
    sleep_ms(300);   /* the four above closed on the loop */
    for (int i = 0; i < 4; i++) held[i] = client_connect(port);
    sleep_ms(600);   /* the loop's sweep has counted them (every 250 ms) */
    s = client_connect(port);
    t0 = now_ms();
    char body[64] = "";
    if (s != CLIENT_BAD) send_all(s, req, (int)sizeof(req) - 1);
    status = s != CLIENT_BAD && readable(s, 3000) ? read_status(s, NULL, body, (int)sizeof(body)) : -1;
    elapsed = now_ms() - t0;
    /* One of the silent ones was closed for it (the oldest on the loop
     * thread it went to): readable, at its end. */
    int evicted = 0;
    for (int i = 0; i < 4; i++) {
        char b;
        if (held[i] != CLIENT_BAD && readable(held[i], i == 0 ? 500 : 0) &&
            recv(held[i], &b, 1, 0) <= 0)
            evicted = 1;
    }
    if (s != CLIENT_BAD) client_close(s);
    for (int i = 0; i < 4; i++) if (held[i] != CLIENT_BAD) client_close(held[i]);
    loop_gateway_free(g);
    if (ran) report("loop slots full of silent connections", status == 200 && evicted && elapsed < 1000,
                    elapsed, status, evicted ? "a silent one closed" : "none closed");
}
#endif

/* The parking lot's bound, 3 here. Each connection asks one request, which
 * is answered, and then waits: the server parks it, so it waits without a
 * worker. A fourth parked closes the one idle longest. The lot declined it
 * before, and a declined connection waited on its worker, so idle clients
 * past the bound held the workers one each. */
static void round_park(void) {
    const char* label = "park bound";
    HttpServer* server = http_server_create(0);
    if (!server) { report(label, 0, -1, -1, "server_create"); return; }
    http_server_get(server, "/ping", ping_handler, NULL);
    const char* err = http_server_set_keepalive_raw(server, 1, 0, 10000LL * 1000000LL);
    if (!*err) err = http_server_set_park_max_connections_raw(server, 3);
    if (*err || http_server_bind_raw(server, "127.0.0.1", 0) < 0 ||
        http_server_start_background_raw(server) < 0) {
        report(label, 0, -1, -1, *err ? err : "bind or start");
        http_server_free(server);
        return;
    }
    int port = http_server_port(server);
    client_sock held[4];
    int served = 0;
    static const char req[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    for (int i = 0; i < 4; i++) {
        held[i] = client_connect(port);
        if (held[i] == CLIENT_BAD) continue;
        send_all(held[i], req, (int)sizeof(req) - 1);
        char body[32] = "";
        if (read_status(held[i], NULL, body, (int)sizeof(body)) == 200) served++;
        sleep_ms(300);   /* parked by now */
    }
    /* The first is closed for the fourth: readable, at its end. The others
     * are still open. */
    char b;
    int first_closed = held[0] != CLIENT_BAD && readable(held[0], 1500) &&
                       recv(held[0], &b, 1, 0) <= 0;
    int others_open = 1;
    for (int i = 1; i < 4; i++)
        if (held[i] == CLIENT_BAD || readable(held[i], 100)) others_open = 0;
    for (int i = 0; i < 4; i++) if (held[i] != CLIENT_BAD) client_close(held[i]);
    http_server_free(server);
    char note[96];
    snprintf(note, sizeof(note), "%d served, first closed=%d, the rest open=%d",
             served, first_closed, others_open);
    report(label, served == 4 && first_closed && others_open, 0, 0, note);
}

/* A connection to `port` on ::1, or CLIENT_BAD. */
static client_sock client_connect6(int port) {
    client_sock s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s == CLIENT_BAD) return CLIENT_BAD;
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((unsigned short)port);
    addr.sin6_addr.s6_addr[15] = 1;   /* ::1 */
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        client_close(s);
        return CLIENT_BAD;
    }
    return s;
}

/* The per-address limit, 2 here, on a server bound to `host`: two
 * connections from the address, each asking one request and kept, then a
 * third answered 503; once the first closes, a fourth is served again. On
 * ::1 the address counts by its /64. */
static void round_per_ip_on(HttpServer* server, const char* label, int v6) {
    int port = http_server_port(server);
    static const char req[] = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    client_sock held[2];
    int served = 0;
    for (int i = 0; i < 2; i++) {
        held[i] = v6 ? client_connect6(port) : client_connect(port);
        if (held[i] == CLIENT_BAD) continue;
        send_all(held[i], req, (int)sizeof(req) - 1);
        if (readable(held[i], 3000) && read_status(held[i], NULL, NULL, 0) == 200) served++;
    }
    client_sock s = v6 ? client_connect6(port) : client_connect(port);
    int third = -1;
    if (s != CLIENT_BAD) {
        send_all(s, req, (int)sizeof(req) - 1);
        third = readable(s, 3000) ? read_status(s, NULL, NULL, 0) : -1;
        client_close(s);
    }
    if (held[0] != CLIENT_BAD) client_close(held[0]);
    sleep_ms(300);   /* the server saw it go */
    s = v6 ? client_connect6(port) : client_connect(port);
    int fourth = -1;
    if (s != CLIENT_BAD) {
        send_all(s, req, (int)sizeof(req) - 1);
        fourth = readable(s, 3000) ? read_status(s, NULL, NULL, 0) : -1;
        client_close(s);
    }
    if (held[1] != CLIENT_BAD) client_close(held[1]);
    char note[96];
    snprintf(note, sizeof(note), "%d served, a third %d, after one closed %d", served, third, fourth);
    report(label, served == 2 && third == 503 && fourth == 200, 0, third, note);
}

static void round_per_ip(int v6) {
    const char* label = v6 ? "per ip ::1" : "per ip 127.0.0.1";
    HttpServer* server = http_server_create(0);
    if (!server) { report(label, 0, -1, -1, "server_create"); return; }
    http_server_get(server, "/ping", ping_handler, NULL);
    const char* err = http_server_set_keepalive_raw(server, 1, 0, 10000LL * 1000000LL);
    if (!*err) err = http_server_set_max_connections_per_ip_raw(server, 2, 64);
    if (!*err && *http_server_set_max_connections_per_ip_raw(server, 2, 129) == '\0')
        err = "a 129 bit prefix was accepted";
    if (*err) { report(label, 0, -1, -1, err); http_server_free(server); return; }
    int bound = v6 ? http_server_bind_raw(server, "::1", 0)
                   : http_server_bind_raw(server, "127.0.0.1", 0);
    if (bound < 0 || http_server_start_background_raw(server) < 0) {
        if (v6) printf("round[%s]: skipped, no IPv6 loopback here\n", label);
        else report(label, 0, -1, -1, "bind or start");
        http_server_free(server);
        return;
    }
    round_per_ip_on(server, label, v6);
    http_server_free(server);
}

#ifndef _WIN32
/* The same on the proxy event loop: a gateway with the per-address limit. */
static void round_loop_per_ip(void) {
    const char* label = "loop per ip";
    g_loop_per_ip = 2;
    Gateway g = loop_gateway(label, 0, 0);
    g_loop_per_ip = 0;
    if (!g.server) return;
    round_per_ip_on(g.server, label, 0);
    on_loop(g, label);
    loop_gateway_free(g);
}

/* Out of descriptors. The process's descriptor limit is lowered to just past
 * what is open, clients connect until socket() fails, and then one more
 * connects on a descriptor held back for it, so its connection waits in the
 * listener's queue with nothing left for the server's accept(). The
 * listener stays readable and accept() keeps failing with EMFILE: the
 * accept loop went straight back to poll and accept, a core spinning for as
 * long as the client held its connections. Over 2 s of that, the process
 * must use well under a second of CPU. Skipped where the limit does not
 * bind sockets (Cygwin). */
static void round_descriptors(void) {
    const char* label = "descriptors plain";
    HttpServer* server = http_server_create(0);
    if (!server) { report(label, 0, -1, -1, "server_create"); return; }
    http_server_get(server, "/ping", ping_handler, NULL);
    http_server_set_keepalive_raw(server, 1, 0, 10000LL * 1000000LL);
    if (http_server_bind_raw(server, "127.0.0.1", 0) < 0 ||
        http_server_start_background_raw(server) < 0) {
        report(label, 0, -1, -1, "bind or start");
        http_server_free(server);
        return;
    }
    int port = http_server_port(server);
    sleep_ms(200);

    enum { MAX_HELD = 64 };
    client_sock held[MAX_HELD];
    int n = 0;
    struct rlimit old;
    getrlimit(RLIMIT_NOFILE, &old);
    int spare = dup(0);   /* held back for the last client */
    int next_fd = dup(0);
    if (next_fd >= 0) close(next_fd);
    struct rlimit low = old;
    low.rlim_cur = (rlim_t)(next_fd + 8);
    if (spare < 0 || next_fd < 0 || setrlimit(RLIMIT_NOFILE, &low) != 0) {
        if (spare >= 0) close(spare);
        printf("round[%s]: skipped, the descriptor limit cannot be lowered\n", label);
        http_server_free(server);
        return;
    }
    while (n < MAX_HELD) {
        client_sock s = client_connect(port);
        if (s == CLIENT_BAD) break;
        held[n++] = s;
        sleep_ms(20);   /* the server takes each before the next */
    }
    int enforced = n < MAX_HELD;
    close(spare);
    client_sock last = enforced ? client_connect(port) : CLIENT_BAD;
    sleep_ms(300);   /* the server is at accept(), with no descriptor for it */

    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    sleep_ms(2000);
    getrusage(RUSAGE_SELF, &r1);
    setrlimit(RLIMIT_NOFILE, &old);
    if (last != CLIENT_BAD) client_close(last);
    for (int i = 0; i < n; i++) client_close(held[i]);
    http_server_free(server);
    if (!enforced) {
        printf("round[%s]: skipped, the descriptor limit does not bind sockets here\n", label);
        return;
    }
    int64_t cpu = ((int64_t)r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) * 1000 +
                  ((int64_t)r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1000 +
                  ((int64_t)r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1000 +
                  ((int64_t)r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1000;
    char note[128];
    snprintf(note, sizeof(note), "%d connections held, one queued, %lld ms of CPU in 2 s",
             n, (long long)cpu);
    report(label, last != CLIENT_BAD && cpu < 500, 2000, 0, note);
}
#endif

/* ---- threads, for the rounds that need two things at once ---- */

typedef void (*slow_thread_fn)(void*);
typedef struct { slow_thread_fn fn; void* arg; } SlowThreadArg;
#ifdef _WIN32
typedef HANDLE slow_thread;
static DWORD WINAPI slow_thread_tramp(LPVOID p) {
    SlowThreadArg* a = (SlowThreadArg*)p;
    a->fn(a->arg);
    return 0;
}
static int slow_thread_start(slow_thread* t, SlowThreadArg* a) {
    *t = CreateThread(NULL, 0, slow_thread_tramp, a, 0, NULL);
    return *t ? 0 : -1;
}
static void slow_thread_join(slow_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#include <pthread.h>
typedef pthread_t slow_thread;
static void* slow_thread_tramp(void* p) {
    SlowThreadArg* a = (SlowThreadArg*)p;
    a->fn(a->arg);
    return NULL;
}
static int slow_thread_start(slow_thread* t, SlowThreadArg* a) {
    return pthread_create(t, NULL, slow_thread_tramp, a) == 0 ? 0 : -1;
}
static void slow_thread_join(slow_thread t) { pthread_join(t, NULL); }
#endif

/* A send that fails rather than raising SIGPIPE once the server has gone. */
static int send_nosig(client_sock s, const void* p, int len) {
#ifdef MSG_NOSIGNAL
    return (int)send(s, (const char*)p, len, MSG_NOSIGNAL);
#else
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return (int)send(s, (const char*)p, len, 0);
#endif
}

/* A request body sent in a burst, then a byte every tick: the burst
 * earned the minimum rate's credit for 100 s (1000 bytes a second), and the
 * trickle after it is far below the rate. With the rate window at 1 s the
 * server cuts it off within the window; with the burst's whole credit kept
 * it held the worker past the trickle (#3737). */
static void round_burst_plain(void) {
    const char* label = "burst plain";
    HttpServer* server = start_server(PLAIN, 0, label);
    if (!server) return;
    const char* err = http_server_set_body_rate_window_raw(server, (int64_t)BUDGET_MS * 1000000LL);
    if (*err) { report(label, 0, -1, -1, err); http_server_free(server); return; }
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    static const char head[] = "POST /len HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 200000\r\n\r\n";
    static char burst[100000];
    memset(burst, 'b', sizeof(burst));
    send_all(s, head, (int)sizeof(head) - 1);
    send_all(s, burst, (int)sizeof(burst));
    static char slow[512];
    memset(slow, 'y', sizeof(slow));
    int64_t t0 = now_ms();
    int64_t elapsed = trickle(s, slow, (int)sizeof(slow), t0);
    int status = elapsed >= 0 ? read_status(s, NULL, NULL, 0) : -1;
    client_close(s);
    http_server_free(server);
    expect_cut(label, elapsed, status, 408);
}

/* ---- raw HTTP/2 frames, for the h2 rounds ---- */

static int h2_recv_n(client_sock s, unsigned char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = (int)recv(s, (char*)buf + got, (int)(n - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int h2_send_frame(client_sock s, int type, int flags, unsigned stream,
                         const unsigned char* payload, size_t len) {
    unsigned char h[9];
    h[0] = (unsigned char)(len >> 16);
    h[1] = (unsigned char)(len >> 8);
    h[2] = (unsigned char)len;
    h[3] = (unsigned char)type;
    h[4] = (unsigned char)flags;
    h[5] = (unsigned char)((stream >> 24) & 0x7F);
    h[6] = (unsigned char)(stream >> 16);
    h[7] = (unsigned char)(stream >> 8);
    h[8] = (unsigned char)stream;
    if (send_all(s, h, 9) != 0) return -1;
    return len ? send_all(s, payload, (int)len) : 0;
}

/* One frame: its type, flags, stream and payload (up to cap). The payload
 * length, or -1 on the end, a timeout or an overflow. */
static long h2_read_frame(client_sock s, int* type, int* flags, unsigned* stream,
                          unsigned char* buf, size_t cap) {
    unsigned char h[9];
    if (h2_recv_n(s, h, 9) != 0) return -1;
    size_t len = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
    *type = h[3];
    *flags = h[4];
    *stream = ((unsigned)(h[5] & 0x7F) << 24) | ((unsigned)h[6] << 16) |
              ((unsigned)h[7] << 8) | h[8];
    if (len > cap) return -1;
    if (len && h2_recv_n(s, buf, len) != 0) return -1;
    return (long)len;
}

/* HPACK "literal header field without indexing, new name", lengths under
 * 127, no Huffman. */
static size_t h2_literal(unsigned char* out, const char* name, const char* value) {
    size_t n = 0, nl = strlen(name), vl = strlen(value);
    out[n++] = 0x00;
    out[n++] = (unsigned char)nl;
    memcpy(out + n, name, nl); n += nl;
    out[n++] = (unsigned char)vl;
    memcpy(out + n, value, vl); n += vl;
    return n;
}

static int h2_send_headers(client_sock s, unsigned stream, const char* method,
                           const char* path, int end_stream) {
    unsigned char block[256];
    size_t n = 0;
    n += h2_literal(block + n, ":method", method);
    n += h2_literal(block + n, ":scheme", "http");
    n += h2_literal(block + n, ":path", path);
    n += h2_literal(block + n, ":authority", "127.0.0.1");
    return h2_send_frame(s, 0x1, 0x4 | (end_stream ? 0x1 : 0), stream, block, n);
}

/* A prior-knowledge HTTP/2 connection, the preface and SETTINGS
 * exchanged, or CLIENT_BAD. */
static client_sock h2_connect(int port) {
    client_sock s = client_connect(port);
    if (s == CLIENT_BAD) return CLIENT_BAD;
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    send_all(s, preface, (int)sizeof(preface) - 1);
    h2_send_frame(s, 0x4, 0, 0, NULL, 0);
    static unsigned char buf[65536];
    int type = 0, flags = 0;
    unsigned stream = 0;
    for (;;) {
        long n = h2_read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
        if (n < 0) { client_close(s); return CLIENT_BAD; }
        if (type == 0x4 && !(flags & 0x1)) break;
    }
    h2_send_frame(s, 0x4, 0x1, 0, NULL, 0);
    return s;
}

static long h2_rst_code(const unsigned char* b) {
    return ((long)b[0] << 24) | ((long)b[1] << 16) | ((long)b[2] << 8) | b[3];
}

/* The same burst and trickle on an HTTP/2 stream: 60000 bytes of DATA (the
 * stream's first window), then a byte of DATA every tick. Its request is
 * reset (CANCEL) within its head and body budgets (2 s) and a margin,
 * where the burst's whole credit kept it for 62 s. */
static void round_burst_h2(void) {
    const char* label = "burst h2";
    HttpServer* server = start_server(PLAIN, 1, label);
    if (!server) return;
    const char* err = http_server_set_body_rate_window_raw(server, (int64_t)BUDGET_MS * 1000000LL);
    if (*err) { report(label, 0, -1, -1, err); http_server_free(server); return; }
    client_sock s = h2_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "h2 connect"); http_server_free(server); return; }
    static unsigned char data[15000];
    memset(data, 'b', sizeof(data));
    static unsigned char buf[65536];
    int64_t t0 = now_ms();
    h2_send_headers(s, 1, "POST", "/len", 0);
    for (int i = 0; i < 4; i++) h2_send_frame(s, 0x0, 0, 1, data, sizeof(data));
    long code = -1;
    int type = 0, flags = 0;
    unsigned stream = 0;
    while (now_ms() - t0 < TRICKLE_MS && code < 0) {
        if (readable(s, TICK_MS)) {
            long n = h2_read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
            if (n < 0) break;
            if (type == 0x3 && stream == 1 && n == 4) code = h2_rst_code(buf);
            continue;
        }
        h2_send_frame(s, 0x0, 0, 1, data, 1);
    }
    int64_t elapsed = now_ms() - t0;
    client_close(s);
    http_server_free(server);
    char note[64];
    snprintf(note, sizeof(note), "RST_STREAM %ld", code);
    report(label, code == 0x8 && elapsed < 3000, elapsed, 0, note);
}

/* A whole HTTP/1.0 request shorter than the HTTP/2 preface's 24 bytes, on a
 * server with HTTP/2 on: the probe for the preface read until it had 24
 * bytes, so this waited out the head's deadline and was answered 408. It
 * is answered at once. */
static void round_h2_short(void) {
    const char* label = "h2 probe short request";
    HttpServer* server = start_server(PLAIN, 1, label);
    if (!server) return;
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    static const char req[] = "GET /ping HTTP/1.0\r\n\r\n";
    int64_t t0 = now_ms();
    send_all(s, req, (int)sizeof(req) - 1);
    char buf[512];
    int have = 0;
    while (have < (int)sizeof(buf) - 1 && readable(s, CLIENT_WAIT_MS)) {
        int n = (int)recv(s, buf + have, (int)sizeof(buf) - 1 - have, 0);
        if (n <= 0) break;
        have += n;
        buf[have] = '\0';
        if (strstr(buf, "pong")) break;
    }
    buf[have] = '\0';
    int64_t elapsed = now_ms() - t0;
    client_close(s);
    http_server_free(server);
    int status = have > 12 ? atoi(buf + 9) : -1;
    report(label, status == 200 && strstr(buf, "pong") && elapsed < 500, elapsed, status,
           "an 18-byte HTTP/1.0 request");
}

typedef struct { HttpServer* server; int delay_ms; int64_t took_ms; } StopJob;

static void stop_job(void* p) {
    StopJob* j = (StopJob*)p;
    sleep_ms(j->delay_ms);
    int64_t t = now_ms();
    http_server_stop(j->server);
    j->took_ms = now_ms() - t;
}

/* A stop with an HTTP/2 stream open whose client keeps it going, a byte of
 * DATA and a PING every tick, under budgets of 30 s: the stop sent GOAWAY
 * and waited for the stream, which waited for its client, so the stop
 * returned only once the client gave up. The stream's request is refused
 * (RST_STREAM REFUSED_STREAM) and the stop returns at once. */
static void round_h2_stop(void) {
    const char* label = "h2 stop";
    HttpServer* server = start_server(PLAIN, 1, label);
    if (!server) return;
    const char* err = http_server_set_head_timeout_raw(server, 30000LL * 1000000LL);
    if (!*err) err = http_server_set_body_timeout_raw(server, 30000LL * 1000000LL, MIN_RATE);
    if (*err) { report(label, 0, -1, -1, err); http_server_free(server); return; }
    client_sock s = h2_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "h2 connect"); http_server_free(server); return; }
    static const unsigned char one[1] = { 'x' };
    static const unsigned char ping[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static unsigned char buf[65536];
    h2_send_headers(s, 1, "POST", "/len", 0);
    h2_send_frame(s, 0x0, 0, 1, one, 1);
    StopJob job = { server, 300, -1 };
    SlowThreadArg arg = { stop_job, &job };
    slow_thread t;
    if (slow_thread_start(&t, &arg) != 0) {
        report(label, 0, -1, -1, "thread");
        client_close(s);
        http_server_free(server);
        return;
    }
    int64_t t0 = now_ms();
    long code = -1;
    int type = 0, flags = 0;
    unsigned stream = 0;
    while (now_ms() - t0 < 8000) {
        if (readable(s, TICK_MS)) {
            long n = h2_read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
            if (n < 0) break;   /* closed */
            if (type == 0x3 && stream == 1 && n == 4) code = h2_rst_code(buf);
            continue;
        }
        if (code < 0) h2_send_frame(s, 0x0, 0, 1, one, 1);
        h2_send_frame(s, 0x6, 0, 0, ping, sizeof(ping));
    }
    slow_thread_join(t);
    client_close(s);
    http_server_free(server);
    char note[64];
    snprintf(note, sizeof(note), "the stop took %lld ms, RST_STREAM %ld", (long long)job.took_ms, code);
    report(label, job.took_ms >= 0 && job.took_ms < 1500 && code == 0x7, job.took_ms, 0, note);
}

/* 1 MiB after 1.5 s: past the 600 ms idle timeout. */
static void slow_big_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    static char big[1024 * 1024 + 1];
    if (!big[0]) memset(big, 'q', sizeof(big) - 1);
    sleep_ms(1500);
    http_response_set_status(res, 200);
    http_response_set_body(res, big);
}

/* A handler that runs past the idle timeout and answers 1 MiB, to a client
 * with HTTP/2's first window of 64 KiB that sends WINDOW_UPDATE for each
 * DATA frame it reads. The connection's idle clock ran from the client's
 * last bytes, the request, so the first window went out and then a GOAWAY
 * before the client's WINDOW_UPDATE let the rest follow. All of it
 * arrives. */
static void round_h2_slow_handler(void) {
    const char* label = "h2 slow handler";
    HttpServer* server = start_server(PLAIN, 1, label);
    if (!server) return;
    http_server_get(server, "/slowbig", slow_big_handler, NULL);
    client_sock s = h2_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "h2 connect"); http_server_free(server); return; }
    static unsigned char buf[65536];
    int64_t t0 = now_ms();
    h2_send_headers(s, 1, "GET", "/slowbig", 1);
    long got = 0;
    int ended = 0, goaway = 0;
    int type = 0, flags = 0;
    unsigned stream = 0;
    while (!ended && now_ms() - t0 < 10000) {
        long n = h2_read_frame(s, &type, &flags, &stream, buf, sizeof(buf));
        if (n < 0) break;
        if (type == 0x7) goaway = 1;
        if (type == 0x0 && stream == 1) {
            got += n;
            if (n > 0) {
                unsigned char inc[4] = { (unsigned char)(n >> 24), (unsigned char)(n >> 16),
                                         (unsigned char)(n >> 8), (unsigned char)n };
                h2_send_frame(s, 0x8, 0, 1, inc, 4);
                h2_send_frame(s, 0x8, 0, 0, inc, 4);
            }
            if (flags & 0x1) ended = 1;
        }
    }
    int64_t elapsed = now_ms() - t0;
    client_close(s);
    http_server_free(server);
    char note[96];
    snprintf(note, sizeof(note), "%ld of %d bytes, end_stream=%d goaway=%d", got, 1024 * 1024,
             ended, goaway);
    report(label, ended && got == 1024 * 1024, elapsed, 0, note);
}

/* A client a TLS 1.3 KeyUpdate before its first request, on a server whose
 * idle timeout is 3 s and head budget 1 s: the record carries no data, so
 * the connection is still idle and its request, 1.5 s later, is answered.
 * The head's clock started at the KeyUpdate's bytes, and the request met a
 * 408. */
static void round_key_update(int backend) {
    char label[64];
    snprintf(label, sizeof(label), "key update %s", backend_name[backend]);
    HttpServer* server = start_server(backend, 0, label);
    if (!server) return;
    const char* err = http_server_set_keepalive_raw(server, 1, 0, 3000LL * 1000000LL);
    if (*err) { report(label, 0, -1, -1, err); http_server_free(server); return; }
    client_sock s = client_connect(http_server_port(server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); http_server_free(server); return; }
    void* tls = slow_tls_connect((int)s);
    if (!tls || slow_tls_key_update(tls) != 0) {
        printf("round[%s]: skipped, %s\n", label,
               !slow_tls_available() ? "no TLS client in this build"
               : !tls ? "the TLS handshake failed" : "no TLS 1.3 KeyUpdate");
        if (slow_tls_available() && !tls) g_fail++;
        slow_tls_free(tls);
        client_close(s);
        http_server_free(server);
        return;
    }
    sleep_ms(1500);
    static const char req[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    int64_t t0 = now_ms();
    slow_tls_write(tls, req, (int)sizeof(req) - 1);
    char body[64] = "";
    int status = read_status(s, tls, body, (int)sizeof(body));
    int64_t elapsed = now_ms() - t0;
    slow_tls_free(tls);
    client_close(s);
    http_server_free(server);
    report(label, status == 200 && strcmp(body, "pong") == 0, elapsed, status, body);
}

/* One address past its limit (1 here) is answered 503, and what it sends
 * after is read and dropped for a while so the 503 is not reset away. A
 * client that keeps sending held the worker for 2 s each time, and could
 * open the next at once; the linger is 500 ms, as the event loop's. Timed
 * by the client: its sends fail once the server has closed. */
static void round_per_ip_linger(void) {
    const char* label = "per ip linger";
    HttpServer* server = http_server_create(0);
    if (!server) { report(label, 0, -1, -1, "server_create"); return; }
    http_server_get(server, "/ping", ping_handler, NULL);
    const char* err = http_server_set_keepalive_raw(server, 1, 0, 10000LL * 1000000LL);
    if (!*err) err = http_server_set_max_connections_per_ip_raw(server, 1, 64);
    if (*err || http_server_bind_raw(server, "127.0.0.1", 0) < 0 ||
        http_server_start_background_raw(server) < 0) {
        report(label, 0, -1, -1, *err ? err : "bind or start");
        http_server_free(server);
        return;
    }
    int port = http_server_port(server);
    static const char req[] = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    client_sock held = client_connect(port);
    int first = -1;
    if (held != CLIENT_BAD) {
        send_all(held, req, (int)sizeof(req) - 1);
        first = readable(held, 3000) ? read_status(held, NULL, NULL, 0) : -1;
    }
    client_sock s = client_connect(port);
    int status = -1;
    int64_t elapsed = -1;
    if (s != CLIENT_BAD) {
        status = readable(s, 3000) ? read_status(s, NULL, NULL, 0) : -1;
        int64_t t0 = now_ms();
        while (now_ms() - t0 < 4000) {
            if (send_nosig(s, "x", 1) != 1) { elapsed = now_ms() - t0; break; }
            sleep_ms(100);
        }
        client_close(s);
    }
    if (held != CLIENT_BAD) client_close(held);
    http_server_free(server);
    char note[96];
    snprintf(note, sizeof(note), "first %d, the refused one %d, its sends failed after %lld ms",
             first, status, (long long)elapsed);
    report(label, first == 200 && status == 503 && elapsed >= 0 && elapsed < 1300, elapsed, status, note);
}

#ifndef _WIN32
/* An upstream that answers any request with a 1 GiB Content-Length, sends
 * 64 MiB of it, and then waits: one connection, then it closes. */
typedef struct { int listener; } EndlessUpstream;

static void endless_upstream(void* p) {
    EndlessUpstream* u = (EndlessUpstream*)p;
    int c = (int)accept(u->listener, NULL, NULL);
    if (c < 0) return;
    char head[4096];
    int have = 0;
    while (have < (int)sizeof(head) - 1) {
        int n = (int)recv(c, head + have, 1, 0);
        if (n <= 0) break;
        have += n;
        head[have] = '\0';
        if (have >= 4 && memcmp(head + have - 4, "\r\n\r\n", 4) == 0) break;
    }
    static const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 1073741824\r\n\r\n";
    static char chunk[65536];
    memset(chunk, 'e', sizeof(chunk));
    int ok = send_nosig(c, resp, (int)sizeof(resp) - 1) > 0;
    for (int i = 0; ok && i < 1024; i++) {
        int off = 0;
        while (off < (int)sizeof(chunk)) {
            int n = send_nosig(c, chunk + off, (int)sizeof(chunk) - off);
            if (n <= 0) { ok = 0; break; }
            off += n;
        }
    }
    /* Then nothing, until the gateway lets go or 12 s pass. */
    for (int i = 0; ok && i < 120; i++) {
        char b;
        if (readable(c, 100) && recv(c, &b, 1, 0) <= 0) break;
    }
    close(c);
}

/* A gateway with a 1 MiB body cap before that upstream. The loop's own
 * exchange read the upstream with no limit (the proxy's cap was on the
 * request, which the loop does not read through), so it buffered the
 * 64 MiB and waited for the rest until the upstream timeout, 504 after
 * 10 s. It stops at the cap: 502 response_too_large, at once. */
static void round_loop_endless(void) {
    const char* label = "loop endless upstream";
    int ls = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (ls < 0 || bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 4) != 0 ||
        getsockname(ls, (struct sockaddr*)&a, &al) != 0) {
        report(label, 0, -1, -1, "upstream listen");
        if (ls >= 0) close(ls);
        return;
    }
    EndlessUpstream up = { ls };
    SlowThreadArg arg = { endless_upstream, &up };
    slow_thread t;
    if (slow_thread_start(&t, &arg) != 0) { report(label, 0, -1, -1, "thread"); close(ls); return; }
    g_gw_upstream_port = ntohs(a.sin_port);
    g_gw_body_cap = 1024 * 1024;
    Gateway g = loop_gateway(label, 0, 0);
    g_gw_upstream_port = 0;
    g_gw_body_cap = 0;
    char head[2048] = "";
    int64_t elapsed = -1;
    if (g.server) {
        client_sock s = client_connect(http_server_port(g.server));
        if (s != CLIENT_BAD) {
            static const char req[] = "GET /big HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
            int64_t t0 = now_ms();
            send_all(s, req, (int)sizeof(req) - 1);
            int have = 0;
            while (have < (int)sizeof(head) - 1 && readable(s, 12000)) {
                int n = (int)recv(s, head + have, (int)sizeof(head) - 1 - have, 0);
                if (n <= 0) break;
                have += n;
                head[have] = '\0';
                if (strstr(head, "\r\n\r\n")) break;
            }
            elapsed = now_ms() - t0;
            client_close(s);
        }
    }
    int ran = g.server && on_loop(g, label);
    loop_gateway_free(g);
    shutdown(ls, SHUT_RDWR);
    slow_thread_join(t);
    close(ls);
    if (!ran) return;
    int status = strncmp(head, "HTTP/1.1 ", 9) == 0 ? atoi(head + 9) : -1;
    report(label, status == 502 && strstr(head, "response_too_large") && elapsed < 3000,
           elapsed, status, strstr(head, "response_too_large") ? "response_too_large" : "");
}

/* The burst and trickle through the loop: 408 within the rate window. */
static void round_loop_burst(void) {
    const char* label = "loop burst";
    g_gw_rate_window = BUDGET_MS;
    Gateway g = loop_gateway(label, 0, 0);
    g_gw_rate_window = 0;
    if (!g.server) return;
    client_sock s = client_connect(http_server_port(g.server));
    if (s == CLIENT_BAD) { report(label, 0, -1, -1, "connect"); loop_gateway_free(g); return; }
    static const char head[] = "POST /len HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 200000\r\n\r\n";
    static char burst[100000];
    memset(burst, 'b', sizeof(burst));
    send_all(s, head, (int)sizeof(head) - 1);
    send_all(s, burst, (int)sizeof(burst));
    static char slow[512];
    memset(slow, 'y', sizeof(slow));
    int64_t t0 = now_ms();
    int64_t elapsed = trickle(s, slow, (int)sizeof(slow), t0);
    int status = elapsed >= 0 ? read_status(s, NULL, NULL, 0) : -1;
    client_close(s);
    int ran = on_loop(g, label);
    loop_gateway_free(g);
    if (ran) expect_cut(label, elapsed, status, 408);
}
#endif

int http_slow_clients_probe(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const char* cert = getenv("SLOW_PROBE_CERT");
    const char* key = getenv("SLOW_PROBE_KEY");
    if (cert && *cert && key && *key) { g_cert = cert; g_key = key; }
    const char* only = getenv("SLOW_PROBE_ONLY");   /* one round's name, to run it alone */
#define WANT(name) (!only || !*only || strcmp(only, name) == 0)

    if (WANT("handshake")) for (int b = OPENSSL; b <= AETHER; b++) round_handshake(b);
    if (WANT("head")) for (int b = PLAIN; b <= AETHER; b++) round_head(b, 0);
    if (WANT("h2")) round_head(PLAIN, 1);
    if (WANT("body")) for (int b = PLAIN; b <= AETHER; b++) round_body(b, "body");
    if (WANT("stream")) round_body(PLAIN, "stream");
    if (WANT("read")) round_body(PLAIN, "read");
    if (WANT("chunked")) round_body(PLAIN, "chunked");
    if (WANT("stall")) round_body(PLAIN, "stall");
    if (WANT("rate")) for (int b = PLAIN; b <= AETHER; b++) round_rate(b);
    if (WANT("sleep")) round_sleep();
    if (WANT("park")) round_park();
    if (WANT("perip")) { round_per_ip(0); round_per_ip(1); }
    if (WANT("burst")) { round_burst_plain(); round_burst_h2(); }
    if (WANT("h2short")) round_h2_short();
    if (WANT("h2stop")) round_h2_stop();
    if (WANT("h2slow")) round_h2_slow_handler();
    if (WANT("keyupdate")) for (int b = OPENSSL; b <= AETHER; b++) round_key_update(b);
    if (WANT("linger")) round_per_ip_linger();
#ifndef _WIN32
    if (WANT("loop")) {
        round_loop_slow("head");
        round_loop_slow("body");
        round_loop_slow("handshake");
        round_loop_rate();
        round_loop_slots();
        round_loop_burst();
        round_loop_endless();
    }
    if (WANT("perip")) round_loop_per_ip();
    if (WANT("descriptors")) round_descriptors();
    if (g_upstream) http_server_free(g_upstream);
#else
    printf("loop rounds: skipped, the proxy event loop runs on Linux and macOS only\n");
#endif

    printf("rounds passed=%d failed=%d\n", g_pass, g_fail);
    if (g_fail == 0 && g_pass > 0) printf("http_server_slow_clients passed\n");
    return 0;
}
