/* #2910: a server owns the state of the access logger and the metrics
 * collector it registers, and frees it with the server. Each server here is
 * given both (metrics twice), serves requests that make the collector record
 * routes, and is freed; leaks(1) checks nothing stays behind. */

#include <stdio.h>
#include <string.h>

#include "aether_http_server.h"

#if defined(_WIN32)

int http_hook_state_probe(const char* log_path) {
    (void)log_path;
    printf("http_server_hook_state: skipped on Windows\n");
    return 0;
}

#else

#include <sys/socket.h>
#include <unistd.h>

static void ping_handler(HttpRequest* req, HttpServerResponse* res, void* ud) {
    (void)req; (void)ud;
    http_response_set_status(res, 200);
    http_response_set_body(res, "pong");
}

/* One request over a socket pair, served on this thread. */
static int serve(HttpServer* s, const char* path) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    char req[256];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: x\r\n\r\n", path);
    if (write(sv[0], req, (size_t)n) != n) return 0;
    shutdown(sv[0], SHUT_WR);
    http_server_drain_connection(s, sv[1]);
    char buf[4096];
    ssize_t got = read(sv[0], buf, sizeof(buf) - 1);
    close(sv[0]);
    if (got <= 0) return 0;
    buf[got] = '\0';
    return strstr(buf, " 200 ") != NULL;
}

int http_hook_state_probe(const char* log_path) {
    for (int i = 0; i < 50; i++) {
        HttpServer* s = http_server_create(0);
        if (!s) return 1;
        if (strcmp(http_server_set_access_log_raw(s, "json", log_path), "") != 0) return 2;
        if (strcmp(http_server_set_metrics_raw(s, "/metrics"), "") != 0) return 3;
        if (strcmp(http_server_set_metrics_raw(s, "/metrics2"), "") != 0) return 4;
        http_server_get(s, "/ping", ping_handler, NULL);
        if (!serve(s, "/ping") || !serve(s, "/ping?k=1") || !serve(s, "/metrics")) return 5;
        http_server_free(s);
    }
    printf("http_server_hook_state passed\n");
    return 0;
}

#endif
