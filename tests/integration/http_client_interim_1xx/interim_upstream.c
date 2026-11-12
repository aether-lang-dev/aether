/* Answers requests on one connection with interim 1xx responses before the
 * final one, the way a server honouring `Expect: 100-continue` does (Node's
 * http server, Azurite).
 *
 *   continue  "100 Continue", a pause, then "201 Created" + "made"; then a
 *             second request on the same connection, answered "second".
 *   multi     "100 Continue" and "103 Early Hints" (with a Link field) and
 *             the final "200" + "ok", all in one write.
 *   switch    "101 Switching Protocols", which is final: the client hands
 *             it back as it is.
 */
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#define pause_ms(ms) usleep((ms) * 1000)
#else
#define pause_ms(ms) Sleep(ms)
#endif

#include "raw_socket.h"

/* Read one request: its head, and the body its Content-Length declares. */
static int read_request(raw_sock cs) {
    char buf[8192];
    size_t got = 0;
    for (;;) {
        int n = recv(cs, buf + got, (int)(sizeof(buf) - 1 - got), 0);
        if (n <= 0) return -1;
        got += (size_t)n;
        buf[got] = '\0';
        char* end = strstr(buf, "\r\n\r\n");
        if (!end) continue;
        size_t head = (size_t)(end + 4 - buf);
        long cl = 0;
        char* p = strstr(buf, "Content-Length:");
        if (!p) p = strstr(buf, "content-length:");
        if (p) cl = atol(p + 15);
        while (got < head + (size_t)cl) {
            n = recv(cs, buf + got, (int)(sizeof(buf) - 1 - got), 0);
            if (n <= 0) return -1;
            got += (size_t)n;
        }
        return 0;
    }
}

static void say(raw_sock cs, const char* s) { send(cs, s, (int)strlen(s), 0); }

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "continue";
    if (raw_socket_start() != 0) return 1;
    raw_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(ls, (struct sockaddr*)&a, sizeof(a));
    listen(ls, 8);
    socklen_t al = sizeof(a);
    getsockname(ls, (struct sockaddr*)&a, &al);
    printf("%d\n", ntohs(a.sin_port));
    fflush(stdout);
    raw_sock cs = accept(ls, NULL, NULL);
    if (read_request(cs) != 0) return 1;
    if (strcmp(mode, "continue") == 0) {
        say(cs, "HTTP/1.1 100 Continue\r\n\r\n");
        pause_ms(300);
        say(cs, "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\nmade");
        if (read_request(cs) == 0)
            say(cs, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecond");
    } else if (strcmp(mode, "multi") == 0) {
        say(cs, "HTTP/1.1 100 Continue\r\n\r\n"
                "HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\n"
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
    } else {
        say(cs, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x-test\r\nConnection: Upgrade\r\n\r\n");
        pause_ms(300);
    }
    raw_socket_close(cs);
    raw_socket_close(ls);
    return 0;
}
