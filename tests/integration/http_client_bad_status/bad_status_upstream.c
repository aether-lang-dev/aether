/* Serves a deliberately malformed response chosen by argv[1].
 *
 * A status code is exactly three digits. Reading it with atoi accepts anything
 * beginning with one and wraps on overflow, which handed a caller a negative
 * status and let a proxy copy it onto its own reply. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "huge_status";
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=0;
    bind(ls,(struct sockaddr*)&a,sizeof(a)); listen(ls,8);
    socklen_t al=sizeof(a); getsockname(ls,(struct sockaddr*)&a,&al);
    printf("%d\n", ntohs(a.sin_port)); fflush(stdout);
    int cs = accept(ls, NULL, NULL);
    char req[4096]; if (recv(cs, req, sizeof(req), 0) <= 0) return 1;

    if (strcmp(mode, "long_header") == 0) {
        char* big = malloc(70000);
        memset(big, 'C', 69999); big[69999] = 0;
        dprintf(cs, "HTTP/1.1 200 OK\r\nX-Big: %s\r\nContent-Length: 2\r\n\r\nhi", big);
    } else if (strcmp(mode, "many_headers") == 0) {
        dprintf(cs, "HTTP/1.1 200 OK\r\n");
        for (int i = 0; i < 500; i++) dprintf(cs, "X-H%d: v\r\n", i);
        dprintf(cs, "Content-Length: 2\r\n\r\nhi");
    } else if (strcmp(mode, "huge_cl") == 0) {
        dprintf(cs, "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\nhi");
    } else if (strcmp(mode, "neg_cl") == 0) {
        dprintf(cs, "HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\nhi");
    } else {
        dprintf(cs, "HTTP/1.1 999999999999 Weird\r\nContent-Length: 2\r\n\r\nhi");
    }
    close(cs); close(ls); return 0;
}
