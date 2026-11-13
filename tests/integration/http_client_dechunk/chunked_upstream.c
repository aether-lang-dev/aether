/*
 * chunked_upstream.c — a minimal HTTP/1.1 upstream that replies with
 * `Transfer-Encoding: chunked` and NO Content-Length, for the VCR
 * record-mode chunked-decode test (vcr_record_chunked_dechunk_wish.md).
 *
 * The Aether HTTP server always emits Content-Length, so it can't
 * produce a chunked response — hence this raw-socket responder. Binds
 * 127.0.0.1:0, prints the OS-assigned port (one line) to stdout, then
 * loops: accept, drain the request, write the same chunked 200, close.
 * Runs until killed by the test harness.
 *
 * The body is the 19-byte "hello-from-upstream" framed as a single
 * chunk (0x13 = 19) plus the terminating zero chunk — exactly the shape
 * the wish reported being stored raw in the tape.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "raw_socket.h"

int main(void) {
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);       /* Windows has none: send() just fails */
#endif
    if (raw_socket_start() != 0) { perror("socket start"); return 1; }

    raw_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == RAW_SOCK_INVALID) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(0);                 /* OS-assigned */
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0) { perror("bind"); return 1; }
    if (listen(ls, 16) != 0) { perror("listen"); return 1; }

    struct sockaddr_in bound;
    socklen_t bl = sizeof(bound);
    if (getsockname(ls, (struct sockaddr*)&bound, &bl) != 0) { perror("getsockname"); return 1; }
    printf("%d\n", ntohs(bound.sin_port));
    fflush(stdout);

    static const char* RESP =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "13\r\n"
        "hello-from-upstream\r\n"
        "0\r\n"
        "\r\n";

    /* Transfer-Encoding read as one list (#3670): the path picks the
     * fields, and the body is always "5 hello 0" in chunked framing. Where
     * the final coding is not chunked, the body is those bytes as they are,
     * read to the close, Content-Length or not (RFC 9112 6.3). */
    static const struct { const char* path; const char* fields; } TE[] = {
        { "/xchunked",    "Transfer-Encoding: xchunked\r\n" },
        { "/chunked-gzip", "Transfer-Encoding: chunked, gzip\r\nContent-Length: 3\r\n" },
        { "/two-fields",  "Transfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n" },
    };

    for (;;) {
        raw_sock cs = accept(ls, NULL, NULL);
        if (cs == RAW_SOCK_INVALID) continue;
        char buf[4096];
        int got = (int)recv(cs, buf, sizeof(buf) - 1, 0);
        buf[got > 0 ? got : 0] = '\0';
        const char* resp = RESP;
        char made[512];
        for (size_t i = 0; i < sizeof TE / sizeof TE[0]; i++) {
            size_t pl = strlen(TE[i].path);
            if (strncmp(buf, "GET ", 4) == 0 && strncmp(buf + 4, TE[i].path, pl) == 0 &&
                buf[4 + pl] == ' ') {
                snprintf(made, sizeof made,
                         "HTTP/1.1 200 OK\r\nConnection: close\r\n%s\r\n5\r\nhello\r\n0\r\n\r\n",
                         TE[i].fields);
                resp = made;
            }
        }
        (void)raw_send_all(cs, resp, strlen(resp));
        raw_socket_close(cs);
    }
    return 0;
}
