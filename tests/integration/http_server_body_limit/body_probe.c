/* The client for http_server_body_limit (#3740), against server.ae (body
 * limit 100000 bytes).
 *
 *   1. POST /whole with 50000 bytes: buffered and counted, 200 "got 50000";
 *   2. POST /whole declaring 1000000 bytes, with only the head sent: 413 at
 *      once, without the server waiting for (or allocating) the body;
 *   3. POST /stream with 1000000 bytes sent: the streaming reader is not
 *      bounded by the limit, 200 "streamed 1000000".
 *   4. POST /declared declaring 3000000000 and then 6442450944 bytes, with
 *      only the head sent: request_body_length reports each exactly
 *      (#3761), 200 "declared <n>".
 *
 * Usage: body_probe <port>. Prints PASS/FAIL lines; exits 0 when all pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_socket.h"

static int port;
static int failures;

static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static raw_sock dial(void) {
    raw_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == RAW_SOCK_INVALID) return s;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        raw_socket_close(s);
        return RAW_SOCK_INVALID;
    }
    raw_recv_timeout_ms(s, 5000);
    return s;
}

/* POST `path` declaring `declared` body bytes (its decimal text, so a
 * length past 2^31 needs no 64-bit printf on Windows) and sending `sent` of
 * them; the response's status line and body go into `out`. Returns the
 * status, or -1 when no response came. */
static int post_text(const char* path, const char* declared, long sent, char* out, size_t cap) {
    out[0] = '\0';
    raw_sock s = dial();
    if (s == RAW_SOCK_INVALID) return -1;
    raw_sendf(s, "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\n"
                 "Content-Length: %s\r\nConnection: close\r\n\r\n", path, declared);
    if (sent > 0) {
        char* body = (char*)malloc((size_t)sent);
        if (!body) { raw_socket_close(s); return -1; }
        memset(body, 'x', (size_t)sent);
        raw_send_all(s, body, (size_t)sent);
        free(body);
    }
    size_t n = 0;
    while (n < cap - 1) {
        int r = (int)recv(s, out + n, (int)(cap - 1 - n), 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out[n] = '\0';
    raw_socket_close(s);
    if (n < 12 || strncmp(out, "HTTP/1.1 ", 9) != 0) return -1;
    return atoi(out + 9);
}

static int post(const char* path, long declared, long sent, char* out, size_t cap) {
    char text[24];
    snprintf(text, sizeof(text), "%ld", declared);
    return post_text(path, text, sent, out, cap);
}

static const char* body_of(const char* resp) {
    const char* b = strstr(resp, "\r\n\r\n");
    return b ? b + 4 : "";
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    port = atoi(argv[1]);
    if (raw_socket_start() != 0) return 2;
    char resp[4096];

    int st = post("/whole", 50000, 50000, resp, sizeof(resp));
    check(st == 200 && strcmp(body_of(resp), "got 50000") == 0,
          "a 50000-byte body under the limit is buffered (200, got 50000)");

    st = post("/whole", 1000000, 0, resp, sizeof(resp));
    check(st == 413, "a body declared past the limit is answered 413 before it is sent");
    if (st != 413) printf("  (status %d: %.200s)\n", st, resp);

    st = post("/stream", 1000000, 1000000, resp, sizeof(resp));
    check(st == 200 && strcmp(body_of(resp), "streamed 1000000") == 0,
          "a 1000000-byte body read with request_body_read is streamed whole");
    if (st != 200) printf("  (status %d: %.200s)\n", st, resp);

    /* 4. A declared length past 2^31 (and past 2^32) reaches the handler
     * whole. Only the head is sent. */
    st = post_text("/declared", "3000000000", 0, resp, sizeof(resp));
    check(st == 200 && strcmp(body_of(resp), "declared 3000000000") == 0,
          "a body declared as 3000000000 bytes reports that length");
    if (st != 200 || strcmp(body_of(resp), "declared 3000000000") != 0)
        printf("  (status %d: %.200s)\n", st, resp);
    st = post_text("/declared", "6442450944", 0, resp, sizeof(resp));
    check(st == 200 && strcmp(body_of(resp), "declared 6442450944") == 0,
          "a body declared as 6442450944 bytes reports that length");
    if (st != 200 || strcmp(body_of(resp), "declared 6442450944") != 0)
        printf("  (status %d: %.200s)\n", st, resp);

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: body limit\n");
    return 0;
}
