/* raw_socket.h: the socket calls the raw-socket test fixtures make, on POSIX
 * and on Windows alike.
 *
 * The upstreams and probes beside the std.http tests are written against bare
 * sockets on purpose: they send what no library server or client would (two
 * Content-Lengths, a body split across a pause, a close mid-body), so they
 * cannot be built from std.http itself. They used to include <sys/socket.h>
 * and so skipped on Windows, which left the client and server framing they
 * guard unexercised there.
 *
 * Winsock has the same calls under the same names, apart from the differences
 * this header covers:
 *   - it has to be started (WSAStartup) before the first socket is made;
 *   - a socket is a SOCKET handle, unsigned, so an error is INVALID_SOCKET
 *     rather than a negative int;
 *   - a socket is closed with closesocket(): close() takes a CRT descriptor;
 *   - read() and write() do not work on a socket, so fixtures use recv() and
 *     send(), which both systems have;
 *   - SO_RCVTIMEO takes a DWORD of milliseconds, not a struct timeval;
 *   - dprintf() writes to a descriptor, which a socket is not, so fixtures
 *     format with raw_sendf() instead.
 * setsockopt() takes a const char* option on Windows and a const void* on
 * POSIX, so callers pass (const char*), which both accept. raw_sleep_ms() is
 * here because nanosleep() is not part of the Windows CRT.
 *
 * Link with -lws2_32 on Windows (RAW_SOCKET_LIBS in the test scripts).
 */
#ifndef AETHER_TEST_RAW_SOCKET_H
#define AETHER_TEST_RAW_SOCKET_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET raw_sock;
#define RAW_SOCK_INVALID INVALID_SOCKET

static inline int raw_socket_start(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}
static inline void raw_socket_close(raw_sock s) { closesocket(s); }
static inline void raw_sleep_ms(int ms) { Sleep((DWORD)ms); }
static inline void raw_recv_timeout_ms(raw_sock s, int ms) {
    DWORD tv = (DWORD)ms;           /* Winsock takes milliseconds as a DWORD */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef int raw_sock;
#define RAW_SOCK_INVALID (-1)

static inline int raw_socket_start(void) { return 0; }
static inline void raw_socket_close(raw_sock s) { close(s); }
static inline void raw_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000L * 1000L };
    nanosleep(&ts, NULL);
}
static inline void raw_recv_timeout_ms(raw_sock s, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
}
#endif

/* Sends all of `len` bytes, as a write() to a blocking descriptor does.
 * 0 once they are out, -1 when the peer stopped taking them. */
static inline int raw_send_all(raw_sock s, const char* buf, size_t len) {
    while (len > 0) {
        int n = (int)send(s, buf, (int)len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* printf to a socket, of any length. */
static inline int raw_sendf(raw_sock s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) return -1;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = raw_send_all(s, buf, (size_t)n);
    free(buf);
    return rc;
}

#endif /* AETHER_TEST_RAW_SOCKET_H */
