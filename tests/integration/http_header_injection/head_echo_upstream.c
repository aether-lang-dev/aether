/* Upstream that answers with the request head it was sent.
 *
 * What the client puts on the wire is its contract with every server, and
 * nothing else in the tree asserts it: the keep-alive test passes even when
 * the client sends `Connection: close`, because that upstream holds the
 * socket open regardless of what it is told.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return 1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0) return 1;
    if (listen(ls, 8) != 0) return 1;

    socklen_t al = sizeof(a);
    if (getsockname(ls, (struct sockaddr*)&a, &al) != 0) return 1;
    printf("%d\n", ntohs(a.sin_port));
    fflush(stdout);

    int cs = accept(ls, NULL, NULL);
    if (cs < 0) return 1;

    char req[8192];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        ssize_t n = recv(cs, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[got] = '\0';

    /* The head only, with the blank line dropped, sent back as the body. */
    char* end = strstr(req, "\r\n\r\n");
    size_t head_len = end ? (size_t)(end - req) : got;

    char head[8192];
    if (head_len > sizeof(head) - 1) head_len = sizeof(head) - 1;
    memcpy(head, req, head_len);
    head[head_len] = '\0';

    char resp[16384];
    int rl = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: %d\r\n"
                      "\r\n%s", (int)head_len, head);
    send(cs, resp, (size_t)rl, 0);

    close(cs);
    close(ls);
    return 0;
}
