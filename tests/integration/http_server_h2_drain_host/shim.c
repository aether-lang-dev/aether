/* A host that serves its own connections (http_server_drain_connection,
 * never starting the server) and speaks HTTP/2 on them.
 *
 * The HTTP/2 loop sent GOAWAY once the server was not running, and such a
 * host's server never runs: every HTTP/2 connection got a GOAWAY on its
 * first turn, before its first request, which was then never answered.
 * The stop it meant is `stopping`, which http_server_stop sets.
 *
 * The client sends the preface and its SETTINGS, waits 300 ms (the loop
 * has turned by then), and sends GET /ping on stream 1, which must be
 * answered "pong" before any GOAWAY. Skips without libnghttp2.
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
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

static HttpServer* g_server;
static int g_fd;

static void ping_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    http_response_set_status(res, 200);
    http_response_set_body(res, "pong");
}

#ifdef _WIN32
static DWORD WINAPI drain_thread(LPVOID arg) {
    (void)arg;
    http_server_drain_connection(g_server, g_fd);
    return 0;
}
#else
static void* drain_thread(void* arg) {
    (void)arg;
    http_server_drain_connection(g_server, g_fd);
    return NULL;
}
#endif

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int send_all(sock_t s, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n > 0) {
        int r = (int)send(s, c, (int)n, 0);
        if (r <= 0) return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

static int recv_n(sock_t s, unsigned char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = (int)recv(s, (char*)buf + got, (int)(n - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int send_frame(sock_t s, int type, int flags, unsigned stream,
                      const unsigned char* payload, size_t len) {
    unsigned char h[9] = {
        (unsigned char)(len >> 16), (unsigned char)(len >> 8), (unsigned char)len,
        (unsigned char)type, (unsigned char)flags,
        (unsigned char)((stream >> 24) & 0x7F), (unsigned char)(stream >> 16),
        (unsigned char)(stream >> 8), (unsigned char)stream };
    if (send_all(s, h, 9) != 0) return -1;
    return len ? send_all(s, payload, len) : 0;
}

/* HPACK "literal header field without indexing, new name"; short strings. */
static size_t hpack_literal(unsigned char* out, const char* name, const char* value) {
    size_t n = 0, nl = strlen(name), vl = strlen(value);
    out[n++] = 0x00;
    out[n++] = (unsigned char)nl;
    memcpy(out + n, name, nl); n += nl;
    out[n++] = (unsigned char)vl;
    memcpy(out + n, value, vl); n += vl;
    return n;
}

int http_h2_drain_host_probe(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_server = http_server_create(0);
    if (!g_server) { printf("FAIL: server_create\n"); return 0; }
    const char* err = http_server_set_h2_raw(g_server, 0);
    if (*err) {
        printf("NO_H2 %s\n", err);
        http_server_free(g_server);
        return 0;
    }
    http_server_set_keepalive_raw(g_server, 1, 0, 600LL * 1000000LL);
    http_server_get(g_server, "/ping", ping_handler, NULL);

    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (ls == SOCK_BAD || bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, (struct sockaddr*)&a, &al) != 0) {
        printf("FAIL: listen\n");
        return 0;
    }
    sock_t c = socket(AF_INET, SOCK_STREAM, 0);
    if (c == SOCK_BAD || connect(c, (struct sockaddr*)&a, sizeof(a)) != 0) {
        printf("FAIL: connect\n");
        return 0;
    }
    sock_t srv = accept(ls, NULL, NULL);
    sock_close(ls);
    if (srv == SOCK_BAD) { printf("FAIL: accept\n"); return 0; }
#ifdef _WIN32
    DWORD tv = 3000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv = { 3, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    /* The host hands the connection to the server, on a thread of its own. */
    g_fd = (int)srv;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, drain_thread, NULL, 0, NULL);
#else
    pthread_t th;
    pthread_create(&th, NULL, drain_thread, NULL);
#endif

    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    send_all(c, preface, sizeof(preface) - 1);
    send_frame(c, 0x4, 0, 0, NULL, 0);
    sleep_ms(300);
    unsigned char block[128];
    size_t n = 0;
    n += hpack_literal(block + n, ":method", "GET");
    n += hpack_literal(block + n, ":scheme", "http");
    n += hpack_literal(block + n, ":path", "/ping");
    n += hpack_literal(block + n, ":authority", "127.0.0.1");
    send_frame(c, 0x1, 0x4 | 0x1, 1, block, n);

    int answered = 0, goaway_first = 0;
    static unsigned char buf[65536];
    for (;;) {
        unsigned char h[9];
        if (recv_n(c, h, 9) != 0) break;
        size_t len = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
        unsigned stream = ((unsigned)(h[5] & 0x7F) << 24) | ((unsigned)h[6] << 16) |
                          ((unsigned)h[7] << 8) | h[8];
        if (len > sizeof(buf) || (len && recv_n(c, buf, len) != 0)) break;
        if (h[3] == 0x4 && !(h[4] & 0x1)) send_frame(c, 0x4, 0x1, 0, NULL, 0);
        if (h[3] == 0x7 && !answered) goaway_first = 1;
        if (h[3] == 0x0 && stream == 1 && len == 4 && memcmp(buf, "pong", 4) == 0) {
            answered = 1;
            break;
        }
    }
    sock_close(c);
#ifdef _WIN32
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    http_server_free(g_server);

    printf("  answered=%d goaway_before_answer=%d\n", answered, goaway_first);
    if (answered && !goaway_first) printf("http_server_h2_drain_host passed\n");
    else printf("FAIL: a host's HTTP/2 connection got GOAWAY before its first request\n");
    return 0;
}
