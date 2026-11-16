/* #3703: the TLS handshake had no time limit. http_server_drain_connection
 * shook hands before conn_serve put the receive timeout on the socket, so a
 * client that sent a few bytes of a ClientHello and stopped held the worker
 * (a pool thread, or an actor-mode worker's core) for as long as it kept the
 * connection open: a slowloris on the TLS port.
 *
 * Each round: a server with a 300 ms keep-alive idle timeout (its receive
 * timeout), a client that sends three bytes and stalls, and the time until
 * the server closes the connection. Plain (three bytes of a request head,
 * which the receive timeout already closed: the control) and over TLS through
 * each backend the build has (the first three bytes of a TLS record); on the
 * thread pool everywhere, and in actor dispatch mode where it exists (POSIX).
 * The server must close within ROUND_LIMIT_MS; the client gives up at 5 s,
 * which is where the old code's TLS rounds end, still open. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "aether_http_server.h"
#include "multicore_scheduler.h"
#include "aether_send_message.h"

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
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int client_sock;
#define CLIENT_BAD (-1)
#endif

/* Defined in aether_http_server.c; std.http declares it as an extern. */
int http_server_port(HttpServer* server);

#define IDLE_MS 300
#define CLIENT_WAIT_MS 5000
#define ROUND_LIMIT_MS 2500   /* a few times IDLE_MS: the close, not the client's give-up */

static int64_t now_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
#ifdef _WIN32
    DWORD tv = CLIENT_WAIT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv = {CLIENT_WAIT_MS / 1000, (CLIENT_WAIT_MS % 1000) * 1000};
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

/* Whether the last failed recv was the client's own timeout, not the
 * server's close (a reset, as the unread bytes make it). */
static int client_timed_out(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static void ping_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    http_response_set_status(res, 200);
    http_response_set_body(res, "pong");
}

#ifndef _WIN32
static HttpServer* g_actor_server = NULL;

/* The actor-mode worker step a program registers. */
static void worker_step(void* self) {
    ActorBase* actor = (ActorBase*)self;
    Message msg;
    if (!mailbox_receive(&actor->mailbox, &msg)) return;
    int fd = -1;
    if (msg.type == MSG_HTTP_CONNECTION && aether_message_payload(&msg)) {
        fd = ((HttpConnectionMessage*)aether_message_payload(&msg))->client_fd;
    }
    aether_message_free_payload(&msg);
    if (fd >= 0) http_server_drain_connection(g_actor_server, fd);
}
#endif

/* One round: 1 passed, 0 failed, -1 skipped (a TLS backend this build has
 * not, or actor mode where there is none). */
static int round_once(int actor_mode, const char* backend, const char* cert, const char* key) {
    char label[64];
    snprintf(label, sizeof(label), "%s %s", actor_mode ? "actor" : "pool", backend ? backend : "plain");
#ifdef _WIN32
    if (actor_mode) return -1;   /* actor dispatch mode is POSIX-only */
#endif
    HttpServer* server = http_server_create(0);
    if (!server) { printf("FAIL: [%s] server_create\n", label); return 0; }
    http_server_get(server, "/ping", ping_handler, NULL);
    http_server_set_keepalive_raw(server, 1, 0, (int64_t)IDLE_MS * 1000000LL);
    if (backend) {
        const char* err = http_server_set_tls_raw(server, cert, key);
        if (!err || !*err) err = http_server_set_tls_backend_raw(server, backend);
        if (err && *err) {
            printf("round[%s]: skipped, %s\n", label, err);
            http_server_free(server);
            return -1;
        }
    }
#ifndef _WIN32
    if (actor_mode) {
        g_actor_server = server;
        http_server_set_actor_handler(server, worker_step, aether_send_message,
                                      (void* (*)(int, void (*)(void*), size_t))scheduler_spawn_actor,
                                      (void (*)(void*))scheduler_release_actor);
    }
#endif
    if (http_server_bind_raw(server, "127.0.0.1", 0) < 0 ||
        http_server_start_background_raw(server) < 0) {
        printf("FAIL: [%s] bind or start: %s\n", label, http_server_error(server));
        http_server_free(server);
        return 0;
    }
    int port = http_server_port(server);

    client_sock s = client_connect(port);
    if (s == CLIENT_BAD) {
        printf("FAIL: [%s] connect\n", label);
        http_server_free(server);
        return 0;
    }
    const char* stall = backend ? "\x16\x03\x01" : "GET";
    int64_t t0 = now_ms();
    int closed = 0;
    if (send(s, stall, 3, 0) == 3) {
        char buf[256];
        for (;;) {
            int n = (int)recv(s, buf, sizeof(buf), 0);
            if (n > 0) continue;
            closed = n == 0 || !client_timed_out();
            break;
        }
    }
    int64_t elapsed = now_ms() - t0;
    client_close(s);   /* frees a worker the old code still held */
    http_server_free(server);

    int ok = closed && elapsed < ROUND_LIMIT_MS;
    printf("round[%s]: closed=%d elapsed_ms=%lld %s\n", label, closed, (long long)elapsed,
           ok ? "passed" : "FAILED");
    return ok ? 1 : 0;
}

/* Every round; the certificate from the script (TLS_PROBE_CERT,
 * TLS_PROBE_KEY), the TLS rounds skipped without it. */
int http_tls_handshake_timeout_probe(void) {
    const char* cert = getenv("TLS_PROBE_CERT");
    const char* key = getenv("TLS_PROBE_KEY");
    int have_cert = cert && *cert && key && *key;
    static const char* backends[] = {NULL, "openssl", "aether"};
    int ok = 1, tls_runs = 0;
    for (int mode = 0; mode < 2; mode++) {
        for (int b = 0; b < 3; b++) {
            if (backends[b] && !have_cert) continue;
            int r = round_once(mode, backends[b], cert, key);
            if (r == 0) ok = 0;
            if (r >= 0 && backends[b]) tls_runs++;
        }
    }
    printf("tls_runs=%d\n", tls_runs);
    if (ok) printf("http_tls_handshake_timeout passed\n");
    return 0;
}
