/* Upstream that sends a response in two writes with a gap between them.
 *
 * The header block goes out first and the body follows only after a pause, so
 * a client that treats "headers have arrived" as "the response has arrived"
 * returns a truncated body instead of blocking for the rest. On loopback with
 * a small response every write lands in one segment, which is why the other
 * client tests cannot see this: the split has to be forced.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BODY "0123456789abcdefghijklmnopqrstuvwxyz"

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

    char req[4096];
    ssize_t n = recv(cs, req, sizeof(req), 0);
    if (n <= 0) return 1;

    char head[256];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: %d\r\n"
                      "\r\n", (int)strlen(BODY));
    if (send(cs, head, (size_t)hl, 0) != hl) return 1;

    /* Long enough that a client which stops at the headers has certainly
     * returned before the body is sent. */
    struct timespec gap = { 0, 250L * 1000L * 1000L };
    nanosleep(&gap, NULL);

    if (send(cs, BODY, strlen(BODY), 0) != (ssize_t)strlen(BODY)) return 1;

    close(cs);
    close(ls);
    return 0;
}
