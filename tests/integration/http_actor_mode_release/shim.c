/* #2509: in actor dispatch mode the HTTP server spawns a worker actor for
 * each connection and never released it, so every request left an actor
 * allocated and a slot taken in its core's table. This drives a server in
 * that mode over loopback (fresh connections, keep-alive connections, peers
 * that close without sending, garbage requests) and reports how many actors
 * the scheduler holds during and after. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether_http_server.h"
#include "multicore_scheduler.h"
#include "aether_send_message.h"

#if defined(_WIN32)

/* Actor dispatch mode is POSIX-only (the accept poller is). */
int http_actor_release_probe(void) {
    printf("http_actor_mode_release: skipped on Windows\n");
    return 0;
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Defined in aether_http_server.c; std.http declares it as an extern. */
int http_server_port(HttpServer* server);

static HttpServer* g_server = NULL;

static void ping_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    http_response_set_status(res, 200);
    http_response_set_body(res, "pong");
}

/* The worker step a user registers: take the connection message and run the
 * connection through the standard lifecycle. One message per call, as a
 * generated step takes them. */
static void worker_step(void* self) {
    ActorBase* actor = (ActorBase*)self;
    Message msg;
    if (!mailbox_receive(&actor->mailbox, &msg)) return;
    int fd = -1;
    if (msg.type == MSG_HTTP_CONNECTION && msg.payload_ptr) {
        fd = ((HttpConnectionMessage*)msg.payload_ptr)->client_fd;
    }
    if (msg.payload_ptr) aether_free_message(msg.payload_ptr);
    if (fd >= 0) http_server_drain_connection(g_server, fd);
}

static void noop_step(void* self) { (void)self; }

static int registered_actors(void) {
    int total = 0;
    for (int c = 0; c < num_cores; c++) total += atomic_load(&schedulers[c].actor_count);
    return total;
}

static int connect_loopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char* data) {
    size_t len = strlen(data), off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Sends `count` GET /ping requests on one connection (keep-alive for all but
 * the last) and returns how many "pong" bodies came back. */
static int ping_session(int port, int count) {
    int fd = connect_loopback(port);
    if (fd < 0) return 0;
    int answered = 0;
    char buf[8192];
    for (int i = 0; i < count; i++) {
        char req[256];
        snprintf(req, sizeof(req),
                 "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: %s\r\n\r\n",
                 i + 1 < count ? "keep-alive" : "close");
        if (send_all(fd, req) != 0) break;
        /* Read until this response's body has arrived. */
        size_t have = 0;
        int got = 0;
        while (have < sizeof(buf) - 1) {
            ssize_t n = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have] = '\0';
            if (strstr(buf, "\r\n\r\n") && strstr(buf, "pong")) { got = 1; break; }
        }
        if (!got) break;
        answered++;
    }
    close(fd);
    return answered;
}

/* A peer that connects and leaves without a byte, and one that sends a line
 * that is not HTTP: the worker meets EOF or a parse error. */
static void error_session(int port, int garbage) {
    int fd = connect_loopback(port);
    if (fd < 0) return;
    if (garbage) {
        send_all(fd, "THIS IS NOT HTTP\r\n\r\n");
        char buf[512];
        while (recv(fd, buf, sizeof(buf), 0) > 0) {}
    }
    close(fd);
}

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int http_actor_release_probe(void) {
    /* An actor of the program's own that lives throughout: the first actor
     * takes main-thread mode, the first worker ends it and the scheduler
     * threads run the workers. */
    ActorBase* keep = scheduler_spawn_actor(-1, noop_step, sizeof(ActorBase));
    if (!keep) { printf("FAIL: spawn\n"); return 1; }

    g_server = http_server_create(0);
    if (!g_server) { printf("FAIL: server_create\n"); return 1; }
    http_server_get(g_server, "/ping", ping_handler, NULL);
    http_server_set_actor_handler(g_server, worker_step,
                                  aether_send_message,
                                  (void* (*)(int, void (*)(void*), size_t))scheduler_spawn_actor,
                                  (void (*)(void*))scheduler_release_actor);
    if (http_server_bind_raw(g_server, "127.0.0.1", 0) < 0) {
        printf("FAIL: bind\n");
        return 1;
    }
    int port = http_server_port(g_server);
    if (http_server_start_background_raw(g_server) < 0) {
        printf("FAIL: start\n");
        return 1;
    }

    int registered_before = registered_actors();
    int live_before = atomic_load(&g_aether_config.actor_count);
    int max_extra = 0;
    int requests = 0, answered = 0;

    /* Fresh connections, one request each. */
    for (int i = 0; i < 300; i++) {
        answered += ping_session(port, 1);
        requests++;
        int extra = registered_actors() - registered_before;
        if (extra > max_extra) max_extra = extra;
    }
    /* Keep-alive connections: one worker serves several requests. */
    for (int i = 0; i < 40; i++) {
        answered += ping_session(port, 5);
        requests += 5;
        int extra = registered_actors() - registered_before;
        if (extra > max_extra) max_extra = extra;
    }
    /* Workers that end on an error. */
    for (int i = 0; i < 40; i++) {
        error_session(port, i % 2);
        int extra = registered_actors() - registered_before;
        if (extra > max_extra) max_extra = extra;
    }

    /* The last workers finish after their peer saw the close, and a freed
     * actor waits for every core to pass its quiescent point. */
    int registered_after = 0, live_after = 0, pending = 0;
    for (int w = 0; w < 1000; w++) {
        registered_after = registered_actors();
        live_after = atomic_load(&g_aether_config.actor_count);
        pending = scheduler_released_actors_pending();
        if (registered_after == registered_before && live_after == live_before && pending == 0) break;
        sleep_ms(10);
    }

    http_server_stop(g_server);
    sleep_ms(100);

    printf("requests=%d answered=%d max_extra_actors=%d extra_registered_after=%d "
           "extra_live_after=%d released_not_freed=%d\n",
           requests, answered, max_extra, registered_after - registered_before,
           live_after - live_before, pending);
    if (answered == requests && max_extra <= 16 &&
        registered_after == registered_before && live_after == live_before && pending == 0) {
        printf("http_actor_mode_release passed\n");
    }
    return 0;
}

#endif
