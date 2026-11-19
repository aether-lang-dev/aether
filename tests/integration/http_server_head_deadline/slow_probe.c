/* The client for http_server_head_deadline (#3737), against server.ae
 * (idle timeout 1 s per read, head timeout 2 s from the first byte).
 *
 *   1. a head sent a byte every 250 ms for about 0.75 s, then finished:
 *      inside the deadline, answered 200;
 *   2. a head sent a byte every 250 ms, never finished: each byte comes well
 *      inside the 1 s read timeout, so only the deadline can end it. It must
 *      be answered 408 within 4 s of the first byte; the client gives up
 *      sending after 8 s.
 *
 * Usage: slow_probe <port>. Prints PASS/FAIL lines; exits 0 when all pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_socket.h"
#ifndef _WIN32
#include <sys/select.h>
#endif

static int port;
static int failures;

static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static raw_sock dial(void) {
    raw_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == RAW_SOCK_INVALID) return s;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        raw_socket_close(s);
        return RAW_SOCK_INVALID;
    }
    return s;
}

/* Wait up to `ms` for `s` to be readable: 1 readable, 0 not. */
static int readable(raw_sock s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select((int)s + 1, &rf, NULL, NULL, &tv) > 0;
}

/* Send `head` a byte at a time, one every 250 ms, for at most `ticks`
 * bytes past `prefix` (each a padding byte), watching for an answer
 * between them. When `finish` is set the head is ended after the ticks.
 * Returns the status the server answered with (-1 for none) and the
 * number of 250 ms ticks it took in *took. */
static int slow_request(int ticks, int finish, int* took) {
    raw_sock s = dial();
    if (s == RAW_SOCK_INVALID) return -1;
    static const char prefix[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ";
    raw_send_all(s, prefix, sizeof(prefix) - 1);
    char resp[1024];
    int status = -1;
    int t = 0;
    for (; t < ticks; t++) {
        if (readable(s, 250)) break;
        if (send(s, "a", 1, 0) != 1) break;
    }
    if (finish && t == ticks) {
        static const char end[] = "\r\nConnection: close\r\n\r\n";
        raw_send_all(s, end, sizeof(end) - 1);
    }
    *took = t;
    if (readable(s, 3000)) {
        int n = (int)recv(s, resp, (int)sizeof(resp) - 1, 0);
        if (n > 12) {
            resp[n] = '\0';
            if (strncmp(resp, "HTTP/1.1 ", 9) == 0) status = atoi(resp + 9);
        }
    }
    raw_socket_close(s);
    return status;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    port = atoi(argv[1]);
    if (raw_socket_start() != 0) return 2;
    int took = 0;

    int st = slow_request(3, 1, &took);
    check(st == 200, "a head that dribbles for 0.75 s and ends is answered 200");

    st = slow_request(32, 0, &took);
    printf("  the endless head was answered %d after about %d ms\n", st, took * 250);
    check(st == 408 && took <= 16, "a head that never ends is answered 408 within 4 s of its first byte");

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: head deadline\n");
    return 0;
}
