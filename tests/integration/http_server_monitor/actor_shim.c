/* Puts an HTTP server into actor dispatch mode for actor_mode.ae (#3131).
 * Actor dispatch is reached from C, not from Aether code, so the test's
 * program registers its monitor in Aether and calls this to switch modes. */

#include "aether_http_server.h"
#include "multicore_scheduler.h"
#include "aether_send_message.h"

#if defined(_WIN32)

/* Actor dispatch mode is POSIX-only (the accept poller is). */
int monitor_actor_mode(HttpServer* server) {
    (void)server;
    return -1;
}

#else

static HttpServer* g_server = NULL;

/* One connection message per call, run through the standard lifecycle. */
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

int monitor_actor_mode(HttpServer* server) {
    g_server = server;
    http_server_set_actor_handler(server, worker_step,
                                  aether_send_message,
                                  (void* (*)(int, void (*)(void*), size_t))scheduler_spawn_actor,
                                  (void (*)(void*))scheduler_release_actor);
    return 0;
}

#endif
