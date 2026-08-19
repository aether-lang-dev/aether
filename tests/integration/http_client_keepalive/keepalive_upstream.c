/*
 * keepalive_upstream.c — an HTTP/1.1 upstream that accepts exactly ONE
 * connection, for the client connection-reuse test (#1653).
 *
 * It binds 127.0.0.1:0, prints the port, accepts one connection and then
 * CLOSES THE LISTENING SOCKET. Every later request therefore succeeds only
 * if the client reused the connection it already has: a redial is refused by
 * the kernel, which is what makes the test decisive rather than suggestive.
 *
 * Each response body is the request's ordinal on this connection ("1", "2",
 * ...), so the driver can also tell how many requests one connection carried.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0) { perror("bind"); return 1; }
    if (listen(ls, 16) != 0) { perror("listen"); return 1; }

    struct sockaddr_in bound;
    socklen_t bl = sizeof(bound);
    if (getsockname(ls, (struct sockaddr*)&bound, &bl) != 0) { perror("getsockname"); return 1; }
    printf("%d\n", ntohs(bound.sin_port));
    fflush(stdout);

    int cs = accept(ls, NULL, NULL);
    if (cs < 0) { perror("accept"); return 1; }
    close(ls);                      /* no second connection is possible */

    char buf[8192];
    size_t have = 0;
    int served = 0;
    for (;;) {
        ssize_t n = read(cs, buf + have, sizeof(buf) - have - 1);
        if (n <= 0) break;
        have += (size_t)n;
        buf[have] = '\0';
        /* Requests are bodiless here, so the header terminator ends one. */
        char* end;
        while ((end = strstr(buf, "\r\n\r\n")) != NULL) {
            size_t used = (size_t)(end - buf) + 4;
            served++;
            char body[32];
            int blen = snprintf(body, sizeof(body), "%d", served);
            char resp[256];
            int rlen = snprintf(resp, sizeof(resp),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %d\r\n"
                                "\r\n%s", blen, body);
            if (write(cs, resp, (size_t)rlen) != rlen) { close(cs); return 0; }
            memmove(buf, buf + used, have - used);
            have -= used;
            buf[have] = '\0';
        }
    }
    close(cs);
    return 0;
}
