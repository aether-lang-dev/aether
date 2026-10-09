/* raw_exchange: send a file's bytes to 127.0.0.1:<port> unchanged, then save
 * everything the server answers until it closes the connection.
 *
 *   raw_exchange <port> <request-file> <response-file>
 *
 * This is what the server-side std.http tests used `nc 127.0.0.1 <port>` for:
 * a request written byte for byte, malformed on purpose, and the raw response
 * read back off the socket. nc is not on a Windows runner and not on every
 * Linux one either, so those tests skipped wherever it was missing.
 *
 * Like nc, it does not shut down its sending side when the request is out:
 * the requests carry `Connection: close` and the server's close ends the
 * exchange. A receive timeout bounds a server that never closes, which fails
 * the test's assertions rather than hanging it. The files are opened in binary
 * mode so a Windows CRT does not rewrite the CR LF line endings under test.
 *
 * Exits 0 once connected (whatever the server then did), 1 when the
 * connection could not be made, 2 on bad arguments or files.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_socket.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <port> <request-file> <response-file>\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[1]);
    FILE* in = fopen(argv[2], "rb");
    if (!in) { perror(argv[2]); return 2; }
    FILE* out = fopen(argv[3], "wb");
    if (!out) { perror(argv[3]); fclose(in); return 2; }

#ifdef SIGPIPE
    /* A server that refuses a request and closes mid-send must leave this
     * process alive to read the refusal. Windows has no SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);
#endif
    if (raw_socket_start() != 0) { fclose(in); fclose(out); return 1; }
    raw_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == RAW_SOCK_INVALID) { fclose(in); fclose(out); return 1; }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        raw_socket_close(s);
        fclose(in);
        fclose(out);
        return 1;
    }
    raw_recv_timeout_ms(s, 10000);

    /* A send that fails part-way means the server stopped reading, which is
     * itself an answer (an over-long request refused early): read what it
     * said rather than give up. */
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (raw_send_all(s, buf, n) != 0) break;
    }
    fclose(in);

    for (;;) {
        int r = (int)recv(s, buf, (int)sizeof(buf), 0);
        if (r <= 0) break;
        fwrite(buf, 1, (size_t)r, out);
    }
    fclose(out);
    raw_socket_close(s);
    return 0;
}
