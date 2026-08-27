/* Answers two requests on one connection. The first response carries two
 * different Content-Length headers; if the client believes the wrong one, the
 * leftover bytes are read as the head of the second response. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char** argv) {
    int extra_body = (argc > 1 && strcmp(argv[1], "extra") == 0);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=0;
    bind(ls,(struct sockaddr*)&a,sizeof(a)); listen(ls,8);
    socklen_t al=sizeof(a); getsockname(ls,(struct sockaddr*)&a,&al);
    printf("%d\n", ntohs(a.sin_port)); fflush(stdout);
    int cs = accept(ls, NULL, NULL);
    char req[4096];
    if (recv(cs, req, sizeof(req), 0) <= 0) return 1;
    const char* r1 = extra_body
        /* One length, and more bytes than it accounts for. */
        ? "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello world"
        /* Two lengths that disagree. */
        : "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 11\r\n\r\nhello world";
    send(cs, r1, strlen(r1), 0);
    if (recv(cs, req, sizeof(req), 0) > 0) {
        const char* r2 = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond";
        send(cs, r2, strlen(r2), 0);
    }
    close(cs); close(ls); return 0;
}
