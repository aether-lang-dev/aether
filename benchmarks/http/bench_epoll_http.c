/**
 * Benchmark: epoll-driven actor HTTP server with adaptive keep-alive
 *
 * Three worker modes:
 *   - keep-alive (default): blocking recv loop, max throughput at low concurrency
 *   - close:                one request per connection, scales to high concurrency
 *   - reactor:              non-blocking, re-registers fd with scheduler I/O poller
 *                           after each response — handles 10K+ concurrent connections
 *                           without blocking any scheduler thread
 *
 * The mode is controlled by a command-line flag: --keepalive, --close, or --reactor
 * Default: keep-alive
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <fcntl.h>
#include "../../std/net/aether_http_server.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/config/aether_optimization_config.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static atomic_int total_requests = 0;
static int use_keepalive = 1;  // default: keep-alive
static int use_reactor = 0;

// ---------------------------------------------------------------------------
// Worker pool
// ---------------------------------------------------------------------------
#define WORKERS_PER_CORE 8

static ActorBase** workers = NULL;
static int worker_count = 0;

static __thread int tls_accept_core = -1;
static __thread int tls_worker_rr = 0;
static atomic_int accept_core_counter = 0;

static void* pick_worker(int preferred_core, void (*step)(void*), size_t size) {
    (void)preferred_core; (void)step; (void)size;
    if (tls_accept_core < 0) {
        tls_accept_core = atomic_fetch_add(&accept_core_counter, 1) % num_cores;
    }
    int base = tls_accept_core * WORKERS_PER_CORE;
    int idx = base + (tls_worker_rr++ % WORKERS_PER_CORE);
    return workers[idx];
}

static void noop_release(void* actor) { (void)actor; }

// ---------------------------------------------------------------------------
// Direct mailbox send
// ---------------------------------------------------------------------------
static void direct_send(void* actor_ptr, void* message_data, size_t message_size) {
    ActorBase* actor = (ActorBase*)actor_ptr;

    void* msg_copy = malloc(message_size);
    if (!msg_copy) return;
    memcpy(msg_copy, message_data, message_size);

    Message msg;
    msg.type = *(int*)message_data;
    msg.sender_id = 0;
    msg.payload_int = 0;
    msg.payload_ptr = msg_copy;
    msg.zerocopy.data = NULL;
    msg.zerocopy.size = 0;
    msg.zerocopy.owned = 0;
    msg._reply_slot = NULL;

    mailbox_send(&actor->mailbox, msg);
    atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Respond to a single request
// ---------------------------------------------------------------------------
static inline int respond(int fd, int keepalive) {
    int count = atomic_fetch_add(&total_requests, 1) + 1;

    char body[128];
    int body_len = snprintf(body, sizeof(body),
        "{\"message\":\"hello\",\"count\":%d}", count);

    char response[512];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Server: Aether/1.0-epoll\r\n"
        "%s"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        keepalive ? "Connection: keep-alive\r\n" : "Connection: close\r\n",
        body_len, body);

    return (int)send(fd, response, resp_len, MSG_NOSIGNAL);
}

// ---------------------------------------------------------------------------
// Worker step — keep-alive mode
// ---------------------------------------------------------------------------
static void worker_step_keepalive(void* self) {
    ActorBase* actor = (ActorBase*)self;
    Message msg;

    while (mailbox_receive(&actor->mailbox, &msg)) {
        if (msg.type != MSG_HTTP_CONNECTION) {
            free(msg.payload_ptr);
            continue;
        }

        HttpConnectionMessage* conn = (HttpConnectionMessage*)msg.payload_ptr;
        int fd = conn->client_fd;
        free(msg.payload_ptr);

        // Set short recv timeout for keep-alive wait
        struct timeval tv = { .tv_sec = 0, .tv_usec = 5000 }; // 5ms
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        for (int ka = 0; ka < 10000; ka++) {
            char buffer[8192];
            int total = 0;
            while (total < (int)sizeof(buffer) - 1) {
                int n = recv(fd, buffer + total, sizeof(buffer) - 1 - total, 0);
                if (n > 0) {
                    total += n;
                    buffer[total] = '\0';
                    if (strstr(buffer, "\r\n\r\n")) break;
                } else {
                    break;
                }
            }

            if (total <= 0) break;

            // Check if client wants close
            if (strstr(buffer, "Connection: close")) {
                respond(fd, 0);
                break;
            }

            respond(fd, 1);

            // If mailbox has pending work, yield this connection
            if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0) {
                break;
            }
        }

        close(fd);
    }
}

// ---------------------------------------------------------------------------
// Worker step — connection close mode
// ---------------------------------------------------------------------------
static void worker_step_close(void* self) {
    ActorBase* actor = (ActorBase*)self;
    Message msg;

    while (mailbox_receive(&actor->mailbox, &msg)) {
        if (msg.type != MSG_HTTP_CONNECTION) {
            free(msg.payload_ptr);
            continue;
        }

        HttpConnectionMessage* conn = (HttpConnectionMessage*)msg.payload_ptr;
        int fd = conn->client_fd;
        free(msg.payload_ptr);

        char buffer[8192];
        int total = 0;
        while (total < (int)sizeof(buffer) - 1) {
            int n = recv(fd, buffer + total, sizeof(buffer) - 1 - total, 0);
            if (n > 0) {
                total += n;
                buffer[total] = '\0';
                if (strstr(buffer, "\r\n\r\n")) break;
            } else if (n == 0) {
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                break;
            }
        }

        if (total > 0) {
            respond(fd, 0);
        }

        shutdown(fd, SHUT_WR);
        char drain[512];
        while (recv(fd, drain, sizeof(drain), 0) > 0) {}
        close(fd);
    }
}

// ---------------------------------------------------------------------------
// Worker step — reactor mode (non-blocking, re-registers fd after each response)
//
// Each step() invocation handles exactly one request then returns. The fd is
// re-registered with the scheduler's per-core I/O poller so the scheduler
// thread is never blocked waiting for the next keep-alive request. When data
// arrives, the scheduler delivers MSG_IO_READY and calls step() again.
//
// This allows the same worker pool to handle 10K+ simultaneous keep-alive
// connections without starving the scheduler.
// ---------------------------------------------------------------------------
static void worker_step_reactor(void* self) {
    ActorBase* actor = (ActorBase*)self;
    Message msg;

    while (mailbox_receive(&actor->mailbox, &msg)) {
        int client_fd = -1;

        if (msg.type == MSG_HTTP_CONNECTION) {
            HttpConnectionMessage* conn = (HttpConnectionMessage*)msg.payload_ptr;
            client_fd = conn->client_fd;
            free(msg.payload_ptr);

            // Ensure fd is non-blocking for reactor pattern
            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags >= 0) fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        } else if (msg.type == MSG_IO_READY) {
            IoReadyMessage* io_msg = (IoReadyMessage*)msg.payload_ptr;
            client_fd = io_msg->fd;
            uint32_t events = io_msg->events;
            free(msg.payload_ptr);

            if (events & AETHER_IO_ERROR) {
                close(client_fd);
                continue;
            }
        } else {
            free(msg.payload_ptr);
            continue;
        }

        // Recv — fd is non-blocking; for MSG_IO_READY, data is guaranteed
        char buffer[8192];
        int total = 0;
        while (total < (int)sizeof(buffer) - 1) {
            int n = recv(client_fd, buffer + total, sizeof(buffer) - 1 - total, MSG_DONTWAIT);
            if (n > 0) {
                total += n;
                buffer[total] = '\0';
                if (strstr(buffer, "\r\n\r\n")) break;
            } else {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // EOF or real error
                    close(client_fd);
                    client_fd = -1;
                }
                break;
            }
        }

        if (client_fd < 0) continue;  // already closed above

        if (total == 0) {
            // No data yet (optimistic path sent us here but data not arrived) —
            // register with poller and wait
            int my_core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
            if (scheduler_io_register(my_core, client_fd, actor, AETHER_IO_READ) != 0)
                close(client_fd);
            continue;
        }

        int want_close = strstr(buffer, "Connection: close") != NULL;
        respond(client_fd, !want_close);

        if (!want_close) {
            // Re-register with the scheduler's per-core poller for the next request.
            // step() runs on the scheduler thread, so io_map access is safe.
            int my_core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
            if (scheduler_io_register(my_core, client_fd, actor, AETHER_IO_READ) != 0)
                close(client_fd);  // coop mode or error — no keep-alive
        } else {
            shutdown(client_fd, SHUT_WR);
            char drain[512];
            while (recv(client_fd, drain, sizeof(drain), 0) > 0) {}
            close(client_fd);
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static HttpServer* server = NULL;

void handle_sigint(int sig) {
    (void)sig;
    if (server) http_server_stop(server);
}

int main(int argc, char** argv) {
    signal(SIGINT, handle_sigint);

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--close") == 0)    { use_keepalive = 0; use_reactor = 0; }
        if (strcmp(argv[i], "--keepalive") == 0) { use_keepalive = 1; use_reactor = 0; }
        if (strcmp(argv[i], "--reactor") == 0)   { use_reactor = 1;   use_keepalive = 0; }
    }

    atomic_store(&g_aether_config.inline_mode_disabled, true);

    scheduler_init(0);

    worker_count = num_cores * WORKERS_PER_CORE;

    void (*step_fn)(void*) = use_reactor    ? worker_step_reactor   :
                             use_keepalive  ? worker_step_keepalive :
                                             worker_step_close;

    const char* mode_name = use_reactor   ? "reactor" :
                            use_keepalive ? "keep-alive" : "close";

    printf("Epoll-actor HTTP server: %d cores, %d workers, mode=%s\n",
           num_cores, worker_count, mode_name);

    scheduler_start();

    workers = malloc(worker_count * sizeof(ActorBase*));
    for (int i = 0; i < worker_count; i++) {
        workers[i] = scheduler_spawn_actor(i % num_cores, step_fn, 0);
        if (!workers[i]) {
            fprintf(stderr, "Failed to spawn worker %d\n", i);
            return 1;
        }
    }

    server = http_server_create(8080);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    server->max_connections = 32768;

    http_server_set_actor_handler(server,
        step_fn,
        (void (*)(void*, void*, size_t))direct_send,
        (void* (*)(int, void (*)(void*), size_t))pick_worker,
        (void (*)(void*))noop_release);

    printf("Starting on :8080\nPress Ctrl+C to stop\n\n");

    if (http_server_start(server) != 0) {
        fprintf(stderr, "Failed to start server\n");
        http_server_free(server);
        return 1;
    }

    printf("\nTotal requests served: %d\n", atomic_load(&total_requests));
    http_server_free(server);
    free(workers);
    return 0;
}
