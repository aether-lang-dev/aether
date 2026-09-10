/**
 * Benchmark: HTTP server with thread-per-connection (current mode)
 *
 * Responds with a simple JSON payload on GET /api/hello
 * Used as baseline before actor-dispatch mode implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "../../std/net/aether_http_server.h"

static int request_count = 0;

void hello_handler(HttpRequest* req, HttpServerResponse* res, void* user_data) {
    (void)req;
    (void)user_data;
    int count = __sync_add_and_fetch(&request_count, 1);
    char json[128];
    snprintf(json, sizeof(json), "{\"message\":\"hello\",\"count\":%d}", count);
    http_response_json(res, json);
}

static HttpServer* server = NULL;

void handle_sigint(int sig) {
    (void)sig;
    if (server) {
        http_server_stop(server);
    }
}

int main(void) {
    signal(SIGINT, handle_sigint);

    server = http_server_create(8080);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    http_server_get(server, "/api/hello", hello_handler, NULL);

    printf("Thread-per-connection HTTP server starting on :8080\n");

    // http_server_start_raw() calls bind internally
    if (http_server_start_raw(server) != 0) {
        fprintf(stderr, "Failed to start server\n");
        http_server_free(server);
        return 1;
    }

    printf("\nTotal requests served: %d\n", request_count);
    http_server_free(server);
    return 0;
}
