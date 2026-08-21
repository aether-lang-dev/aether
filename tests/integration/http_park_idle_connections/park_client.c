/*
 * park_client.c — hold N keep-alive connections open at once and serve a
 * request on every one of them, for the connection-parking test (#1663).
 *
 * A worker owns its connection for that connection's whole life, so before
 * parking the number a server could hold open was the worker count
 * (cores * 2, capped at 64): past that, connections that never reached a
 * worker stalled their client, and 200 concurrent keep-alive clients measured
 * 99 rps against 30,000 after. The property under test is exactly that, so
 * the client opens far more connections than any plausible worker count,
 * keeps them all open, and requires every one to answer.
 *
 * Two rounds on the same sockets: the first proves they are served, the
 * second proves they were still usable afterwards, which is what "kept alive"
 * has to mean.
 *
 * Prints one line per round; exits non-zero if any connection fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

static int dial(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    struct timeval tv = { .tv_sec = 20, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

/* One request/response on `fd`. Returns 0 when a 200 with the expected body
 * came back whole. */
static int exchange(int fd) {
    static const char* req = "GET / HTTP/1.1\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
    size_t len = strlen(req);
    if (write(fd, req, len) != (ssize_t)len) return -1;

    char buf[4096];
    size_t have = 0;
    char* hend = NULL;
    while (have < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) return -1;
        have += (size_t)n;
        buf[have] = '\0';
        hend = strstr(buf, "\r\n\r\n");
        if (hend) break;
    }
    if (!hend) return -1;
    if (strncmp(buf, "HTTP/1.1 200", 12) != 0) return -1;

    /* Drain exactly the declared body so the socket is left at a message
     * boundary and the next round starts clean. */
    long clen = 0;
    for (char* line = buf; line && line < hend; ) {
        char* eol = strstr(line, "\r\n");
        if (!eol || eol > hend) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) clen = strtol(line + 15, NULL, 10);
        line = eol + 2;
    }
    size_t header_bytes = (size_t)(hend + 4 - buf);
    size_t body_have = have - header_bytes;
    while ((long)body_have < clen) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return -1;
        body_have += (size_t)n;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <port> <connections>\n", argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int port = atoi(argv[1]);
    int n = atoi(argv[2]);
    if (n <= 0) return 2;

    int* fds = (int*)calloc((size_t)n, sizeof(int));
    if (!fds) return 2;

    int opened = 0;
    for (int i = 0; i < n; i++) {
        fds[i] = dial(port);
        if (fds[i] < 0) {
            fprintf(stderr, "connection %d of %d refused: %s\n", i + 1, n, strerror(errno));
            return 1;
        }
        opened++;
    }
    printf("opened %d\n", opened);

    for (int round = 1; round <= 2; round++) {
        int served = 0;
        for (int i = 0; i < n; i++) {
            if (exchange(fds[i]) == 0) served++;
            else fprintf(stderr, "round %d: connection %d failed\n", round, i + 1);
        }
        printf("round%d served %d of %d\n", round, served, n);
        if (served != n) { for (int i = 0; i < n; i++) close(fds[i]); free(fds); return 1; }
    }

    for (int i = 0; i < n; i++) close(fds[i]);
    free(fds);
    return 0;
}
