/* The server for http_client_max_response (#3750): responses no well-behaved
 * server sends, over a raw socket, one connection at a time.
 *
 *   GET /big/<n>        200 with a Content-Length of n and n bytes of 'b'
 *   GET /chunked/<n>/<k> 200 with n bytes of 'c', chunked, k bytes a chunk
 *   GET /endless_head   a status line, then header lines without end, 4 MiB
 *                       of them, then the close
 *   GET /drip_head      a status line, then one byte of a header every
 *                       100 ms for 4 s, then the close
 *
 * Usage: resp_server. Prints READY <port>, then serves until killed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_socket.h"

/* The request head, up to its blank line. 0 on success. */
static int read_head(raw_sock c, char* head, size_t cap) {
    size_t n = 0;
    while (n < cap - 1) {
        int r = (int)recv(c, head + n, 1, 0);
        if (r <= 0) return -1;
        n++;
        head[n] = '\0';
        if (n >= 4 && memcmp(head + n - 4, "\r\n\r\n", 4) == 0) return 0;
    }
    return -1;
}

static void serve(raw_sock c) {
    char head[4096];
    if (read_head(c, head, sizeof(head)) != 0) return;
    static char chunk[65536];
    if (strncmp(head, "GET /big/", 9) == 0) {
        long long n = atoll(head + 9);
        raw_sendf(c, "HTTP/1.1 200 OK\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n", n);
        memset(chunk, 'b', sizeof(chunk));
        while (n > 0) {
            size_t take = n < (long long)sizeof(chunk) ? (size_t)n : sizeof(chunk);
            if (raw_send_all(c, chunk, take) != 0) return;   /* the client gave up */
            n -= (long long)take;
        }
    } else if (strncmp(head, "GET /chunked/", 13) == 0) {
        long long n = atoll(head + 13);
        const char* slash = strchr(head + 13, '/');
        long long k = slash ? atoll(slash + 1) : 0;
        if (k <= 0 || k > (long long)sizeof(chunk) - 32) k = 512;
        raw_sendf(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        while (n > 0) {
            long long take = n < k ? n : k;
            int h = snprintf(chunk, sizeof(chunk), "%llx\r\n", take);
            memset(chunk + h, 'c', (size_t)take);
            memcpy(chunk + h + take, "\r\n", 2);
            if (raw_send_all(c, chunk, (size_t)(h + take + 2)) != 0) return;
            n -= take;
        }
        raw_sendf(c, "0\r\n\r\n");
    } else if (strncmp(head, "GET /endless_head ", 18) == 0) {
        raw_sendf(c, "HTTP/1.1 200 OK\r\n");
        memset(chunk, 'a', sizeof(chunk));
        memcpy(chunk, "X-Pad: ", 7);
        chunk[sizeof(chunk) - 2] = '\r';
        chunk[sizeof(chunk) - 1] = '\n';
        for (int i = 0; i < 64; i++)
            if (raw_send_all(c, chunk, sizeof(chunk)) != 0) return;
    } else if (strncmp(head, "GET /drip_head ", 15) == 0) {
        raw_sendf(c, "HTTP/1.1 200 OK\r\nX-Slow: ");
        for (int i = 0; i < 40; i++) {
            raw_sleep_ms(100);
            if (raw_send_all(c, "s", 1) != 0) return;
        }
    } else {
        raw_sendf(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
}

int main(void) {
    if (raw_socket_start() != 0) return 2;
    raw_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == RAW_SOCK_INVALID) return 2;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t al = sizeof(a);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 8) != 0 ||
        getsockname(ls, (struct sockaddr*)&a, &al) != 0) {
        printf("FAIL: could not listen\n");
        return 1;
    }
    printf("READY %d\n", ntohs(a.sin_port));
    fflush(stdout);
    for (;;) {
        raw_sock c = accept(ls, NULL, NULL);
        if (c == RAW_SOCK_INVALID) continue;
        serve(c);
        raw_socket_close(c);
    }
}
