/* #2509: in actor dispatch mode the HTTP server spawns a worker actor for
 * each connection and never released it, so every request left an actor
 * allocated and a slot taken in its core's table. This drives a server in
 * that mode over loopback (fresh connections, keep-alive connections, peers
 * that close without sending, garbage requests) and reports how many actors
 * the scheduler holds during and after. It also checks that the stop closes
 * a connection still waiting for its first bytes (#3204), that a stop
 * waits for the workers still serving (http_actor_stop_waits_probe), and
 * that a stop from an actor on a queued worker's core returns
 * (http_actor_stop_on_core_probe, #3688). */

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
int http_actor_stop_waits_probe(void) { return 0; }
int http_actor_stop_on_core_probe(void) { return 0; }

#else

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

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
    if (msg.type == MSG_HTTP_CONNECTION && aether_message_payload(&msg)) {
        fd = ((HttpConnectionMessage*)aether_message_payload(&msg))->client_fd;
    }
    aether_message_free_payload(&msg);
    if (fd >= 0) http_server_drain_connection(g_server, fd);
}

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

    /* A client that connects and sends nothing waits in the accept poller
     * until its first bytes come. The stop closes it (#3204): it was left
     * open with no owner, and the client read nothing until its own
     * timeout. */
    int silent = connect_loopback(port);
    sleep_ms(200);   /* the accept loop takes it and parks it */

    http_server_stop(g_server);
    sleep_ms(100);

    int silent_closed = 0;
    if (silent >= 0) {
        struct timeval tv = {3, 0};
        setsockopt(silent, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char b;
        ssize_t n = recv(silent, &b, 1, 0);
        silent_closed = n == 0 || (n < 0 && errno == ECONNRESET);
        close(silent);
    }

    /* Freed once stopped: the stop has waited for its workers (see
     * http_actor_stop_waits_probe), so none still reads it. */
    http_server_free(g_server);
    g_server = NULL;

    printf("requests=%d answered=%d max_extra_actors=%d extra_registered_after=%d "
           "extra_live_after=%d released_not_freed=%d silent_closed_at_stop=%d\n",
           requests, answered, max_extra, registered_after - registered_before,
           live_after - live_before, pending, silent_closed);
    if (answered == requests && max_extra <= 16 &&
        registered_after == registered_before && live_after == live_before && pending == 0 &&
        silent_closed) {
        printf("http_actor_mode_release passed\n");
    }
    return 0;
}


/* A stop waits for the worker actors serving connections. The run used to
 * end once its accept loop was over, with workers still in a request, so
 * the stop returned and the program freed the server while a worker still
 * read it (TSan: the free against conn_serve's count, in
 * http_server_multi_accept). A handler that takes 400 ms is in its request
 * when the stop comes: it must have finished, and its response gone out,
 * by the time the stop returns. */
static atomic_int g_slow_started;
static atomic_int g_slow_done;

static void slow_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    atomic_store(&g_slow_started, 1);
    sleep_ms(400);
    http_response_set_status(res, 200);
    http_response_set_body(res, "slow");
    atomic_store(&g_slow_done, 1);
}

int http_actor_stop_waits_probe(void) {
    g_server = http_server_create(0);
    if (!g_server) { printf("FAIL: server_create\n"); return 1; }
    http_server_get(g_server, "/slow", slow_handler, NULL);
    http_server_set_actor_handler(g_server, worker_step,
                                  aether_send_message,
                                  (void* (*)(int, void (*)(void*), size_t))scheduler_spawn_actor,
                                  (void (*)(void*))scheduler_release_actor);
    if (http_server_bind_raw(g_server, "127.0.0.1", 0) < 0) { printf("FAIL: bind\n"); return 1; }
    int port = http_server_port(g_server);
    if (http_server_start_background_raw(g_server) < 0) { printf("FAIL: start\n"); return 1; }

    int fd = connect_loopback(port);
    if (fd < 0 || send_all(fd, "GET /slow HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n") != 0) {
        printf("FAIL: request\n");
        return 1;
    }
    for (int w = 0; w < 500 && !atomic_load(&g_slow_started); w++) sleep_ms(10);
    int started = atomic_load(&g_slow_started);

    http_server_stop(g_server);
    int done_at_stop = atomic_load(&g_slow_done);

    char buf[1024];
    size_t have = 0;
    while (have < sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (n <= 0) break;
        have += (size_t)n;
    }
    buf[have] = '\0';
    int answered = strstr(buf, " 200 ") != NULL && strstr(buf, "slow") != NULL;
    close(fd);

    http_server_free(g_server);
    g_server = NULL;
    printf("handler_started=%d handler_done_when_stop_returned=%d answered=%d\n",
           started, done_at_stop, answered);
    if (started && done_at_stop && answered) printf("http_actor_stop_waits passed\n");
    return 0;
}


/* A stop called from an actor whose core holds a worker not stepped yet
 * (#3688). The run waited for every worker given a connection, and a queued
 * worker can only be stepped by its own core, which a stop called in an
 * actor's handler blocks until the run is over: the run waited for the
 * worker, the worker for the core, the core for the run, and the process
 * hung. The same with a one-core scheduler (the script runs this under
 * `taskset -c 0` where it can) and any queued worker.
 *
 * Here the workers are spawned on core 0 (spawn_on_core0), and an actor on
 * core 0 waits in its step for a connection's worker to be spawned, then
 * stops the server. The stop must return (bounded here: a hang is a
 * failure, not a stuck test), the program frees the server, and the queued
 * worker, stepped once core 0 is free, closes its connection without
 * serving it, with the server still whole for it. */
static atomic_int g_core0_spawns;
static atomic_int g_stop_returned;

static void* spawn_on_core0(int core, void (*step)(void*), size_t size) {
    (void)core;
    void* actor = scheduler_spawn_actor(0, step, size);
    if (actor) atomic_fetch_add(&g_core0_spawns, 1);
    return actor;
}

static void stopper_step(void* self) {
    ActorBase* actor = (ActorBase*)self;
    Message msg;
    if (!mailbox_receive(&actor->mailbox, &msg)) return;
    aether_message_free_payload(&msg);
    for (int w = 0; w < 1000 && atomic_load(&g_core0_spawns) == 0; w++) sleep_ms(5);
    sleep_ms(50);   /* the worker's connection message goes out after its spawn */
    http_server_stop(g_server);
    atomic_store(&g_stop_returned, 1);
    scheduler_release_actor(actor);   /* safe from its own step */
}

int http_actor_stop_on_core_probe(void) {
    g_server = http_server_create(0);
    if (!g_server) { printf("FAIL: server_create\n"); return 1; }
    http_server_get(g_server, "/ping", ping_handler, NULL);
    http_server_set_actor_handler(g_server, worker_step, aether_send_message, spawn_on_core0,
                                  (void (*)(void*))scheduler_release_actor);
    if (http_server_bind_raw(g_server, "127.0.0.1", 0) < 0) { printf("FAIL: bind\n"); return 1; }
    int port = http_server_port(g_server);
    if (http_server_start_background_raw(g_server) < 0) { printf("FAIL: start\n"); return 1; }

    /* The stopper takes core 0 first; then a request, whose worker is
     * spawned on core 0 behind it. */
    ActorBase* stopper = scheduler_spawn_actor(0, stopper_step, sizeof(ActorBase));
    if (!stopper) { printf("FAIL: spawn the stopper\n"); return 1; }
    HttpConnectionMessage go;
    go.type = MSG_HTTP_CONNECTION;
    go.client_fd = -1;
    aether_send_message(stopper, &go, sizeof(go));
    sleep_ms(100);

    int fd = connect_loopback(port);
    if (fd < 0 || send_all(fd, "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n") != 0) {
        printf("FAIL: request\n");
        return 1;
    }

    for (int w = 0; w < 1500 && !atomic_load(&g_stop_returned); w++) sleep_ms(10);
    if (!atomic_load(&g_stop_returned)) {
        printf("FAIL: the stop called on core 0, with a worker queued there, did not return in 15 s\n");
        fflush(stdout);
        _exit(1);   /* core 0 is blocked for good: nothing to clean up */
    }

    /* Freed by the program now; the queued worker still holds the server,
     * and its step (worker_step) still reaches it through g_server, as a
     * program's step reaches the server it captured. */
    http_server_free(g_server);

    /* The worker, stepped once core 0 is free, closes the connection. Its
     * request is unread, so the close resets it: ECONNRESET on Linux and
     * macOS, ECONNABORTED where Winsock is underneath (Cygwin). */
    char buf[256];
    size_t have = 0;
    ssize_t n = 0;
    while (have < sizeof(buf) - 1 && (n = recv(fd, buf + have, sizeof(buf) - 1 - have, 0)) > 0) {
        have += (size_t)n;
    }
    int closed = n == 0 || (n < 0 && (errno == ECONNRESET || errno == ECONNABORTED));
    close(fd);
    printf("spawned_on_core0=%d stop_returned=%d connection_closed=%d bytes=%d\n",
           atomic_load(&g_core0_spawns), atomic_load(&g_stop_returned), closed, (int)have);
    if (closed && have == 0 && atomic_load(&g_core0_spawns) >= 1) {
        printf("http_actor_stop_on_core passed\n");
    }
    return 0;
}

#endif
