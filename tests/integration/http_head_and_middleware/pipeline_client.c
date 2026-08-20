/*
 * pipeline_client.c — one connection, three requests, for the HEAD framing and
 * middleware keep-alive test (#1653).
 *
 * The point is to assert on the wire rather than on a client's phrasing: the
 * first version of this test grepped curl's verbose output for "Re-using
 * existing connection", which is curl's wording and not a promise, and it does
 * not appear on every version. This speaks HTTP/1.1 itself.
 *
 * Sends HEAD /, then GET /, then GET /mw over a single socket, and prints one
 * line per response:
 *
 *     1 status=200 clen=13 body=
 *     2 status=200 clen=13 body=body-from-get
 *     3 status=200 clen=22 body=answered-by-middleware
 *
 * A HEAD response that carried a body would desynchronise the stream and the
 * second line would be wrong or missing, which is exactly the failure being
 * guarded. Exits non-zero if fewer than three responses arrive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static char buf[65536];
static size_t have = 0;

/* Read until `buf` holds a complete response: the header block, plus the body
 * `Content-Length` declares unless `expect_body` is 0 (a HEAD reply). */
static int read_response(int fd, int expect_body, int* status, long* clen, char* body, size_t body_cap) {
    char* hend = NULL;
    for (;;) {
        buf[have] = '\0';
        hend = strstr(buf, "\r\n\r\n");
        if (hend) break;
        if (have >= sizeof(buf) - 1) return -1;
        ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    size_t header_bytes = (size_t)(hend + 4 - buf);
    char saved = *hend;
    *hend = '\0';
    const char* sp = strchr(buf, ' ');
    *status = sp ? atoi(sp + 1) : 0;
    *clen = -1;
    for (char* line = buf; line && *line; ) {
        char* eol = strstr(line, "\r\n");
        if (eol) *eol = '\0';
        if (strncasecmp(line, "Content-Length:", 15) == 0) *clen = strtol(line + 15, NULL, 10);
        if (!eol) break;
        *eol = '\r';
        line = eol + 2;
    }
    *hend = saved;

    size_t want = header_bytes + (expect_body && *clen > 0 ? (size_t)*clen : 0);
    while (have < want) {
        ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    size_t body_len = want - header_bytes;
    if (body_len >= body_cap) body_len = body_cap - 1;
    memcpy(body, buf + header_bytes, body_len);
    body[body_len] = '\0';
    memmove(buf, buf + want, have - want);
    have -= want;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <host> <port>\n", argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 2; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { perror("connect"); return 2; }

    struct { const char* req; int expect_body; } steps[3] = {
        { "HEAD / HTTP/1.1\r\nHost: h\r\n\r\n",    0 },
        { "GET / HTTP/1.1\r\nHost: h\r\n\r\n",     1 },
        { "GET /mw HTTP/1.1\r\nHost: h\r\n\r\n",   1 },
    };
    for (int i = 0; i < 3; i++) {
        size_t len = strlen(steps[i].req);
        if (write(fd, steps[i].req, len) != (ssize_t)len) {
            fprintf(stderr, "write %d failed (connection closed early)\n", i + 1);
            close(fd);
            return 1;
        }
        int status = 0; long clen = 0; char body[4096];
        if (read_response(fd, steps[i].expect_body, &status, &clen, body, sizeof(body)) != 0) {
            fprintf(stderr, "response %d never arrived (connection closed early)\n", i + 1);
            close(fd);
            return 1;
        }
        printf("%d status=%d clen=%ld body=%s\n", i + 1, status, clen, body);
    }
    close(fd);
    return 0;
}
