/* Two upstreams in one, selected by argv[1].
 *
 *   short: declares a Content-Length and closes before delivering it
 *   close: declares no framing at all, so the close IS the framing
 *
 * The pair matters: the fix for the first must not turn the second into an
 * error, because a response with nothing declaring its length is complete
 * exactly when the peer closes.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PART "0123456789"

int main(int argc, char** argv) {
    int truncate_body = (argc > 1 && strcmp(argv[1], "short") == 0);

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
    if (recv(cs, req, sizeof(req), 0) <= 0) return 1;

    if (truncate_body) {
        /* Declares far more than it will send. */
        const char* head = "HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n";
        send(cs, head, strlen(head), 0);
        send(cs, PART, strlen(PART), 0);
    } else {
        const char* head = "HTTP/1.1 200 OK\r\n\r\n";
        send(cs, head, strlen(head), 0);
        send(cs, PART, strlen(PART), 0);
    }

    close(cs);
    close(ls);
    return 0;
}
