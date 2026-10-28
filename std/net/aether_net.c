#include "aether_net.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"
#include "../string/aether_string.h"

#if !AETHER_HAS_NETWORKING
TcpSocket* tcp_connect_raw(const char* h, int p) { (void)h; (void)p; return NULL; }
TcpSocket* tcp_connect_buffered_raw(const char* h, int p, int b) { (void)h; (void)p; (void)b; return NULL; }
int tcp_send_raw(TcpSocket* s, const char* d) { (void)s; (void)d; return -1; }
int tcp_send_n_raw(TcpSocket* s, const char* d, int n) { (void)s; (void)d; (void)n; return -1; }
char* tcp_receive_raw(TcpSocket* s, int m) { (void)s; (void)m; return NULL; }
TcpReceiveResult tcp_receive_n_raw(TcpSocket* s, int m) {
    (void)s; (void)m;
    TcpReceiveResult out = { NULL, 0, "net unavailable" };
    return out;
}
int tcp_send_bytes_raw(TcpSocket* s, const void* d, int n) { (void)s; (void)d; (void)n; return -1; }
int tcp_send_all_raw(TcpSocket* s, const void* d, int n) { (void)s; (void)d; (void)n; return 0; }
int tcp_receive_into_raw(TcpSocket* s, void* b, int c) { (void)s; (void)b; (void)c; return TCP_RECV_CLOSED; }
int tcp_close(TcpSocket* s) { (void)s; return 0; }
TcpServer* tcp_listen_raw(int p) { (void)p; return NULL; }
TcpServer* tcp_listen_on_raw(const char* a, int p) { (void)a; (void)p; return NULL; }
int tcp_server_port_raw(TcpServer* s) { (void)s; return -1; }
int tcp_server_poll_raw(TcpServer* s, int t) { (void)s; (void)t; return -1; }
int tcp_set_nodelay_raw(TcpSocket* s, int on) { (void)s; (void)on; return -1; }
int tcp_set_read_buffer_raw(TcpSocket* s, int b) { (void)s; (void)b; return -1; }
TcpSocket* tcp_accept_raw(TcpServer* s) { (void)s; return NULL; }
int tcp_server_close(TcpServer* s) { (void)s; return 0; }
int tcp_fd_raw(TcpSocket* s) { (void)s; return -1; }
int tcp_server_fd_raw(TcpServer* s) { (void)s; return -1; }
TcpSocket* tcp_socket_from_fd_owned(int fd) { (void)fd; return NULL; }
int tcp_poll_raw(TcpSocket* s, int t) { (void)s; (void)t; return -1; }
int tcp_poll2_raw(TcpSocket* a, TcpSocket* b, int t) { (void)a; (void)b; (void)t; return -1; }
void aether_net_init(void) {}
int aether_net_wouldblock(void) { return 0; }
#else

#include <stdlib.h>
#include <string.h>
#include <stdio.h>   /* snprintf (getaddrinfo port string) */
#include <errno.h>   /* EAGAIN/EWOULDBLOCK: idle-peer vs. closed-peer recv */

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close closesocket
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <sys/time.h>   /* struct timeval: glibc leaks it via other headers, musl does not */
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <netinet/tcp.h>   /* TCP_NODELAY (#2136) */
    #include <poll.h>
    #include <fcntl.h>
#endif
#include "aether_net_addr.h"
#include "../../runtime/utils/aether_thread.h"   /* the family memo's lock, aether_now_ns */
#include <limits.h>

/* recv returned <= 0: distinguish "peer idle, socket still alive"
 * (SO_RCVTIMEO fired, or a nonblocking socket with nothing buffered) from
 * "peer closed / hard error". A 30 s quiet window on a long-lived tunnel is
 * normal and must NOT tear the connection down — see issue #1092. Returns 1
 * for would-block/timeout/interrupt, 0 for a genuine close-or-error. Only
 * meaningful when `received < 0`; an orderly FIN is received == 0. */
int aether_net_wouldblock(void) {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

struct TcpSocket {
    int fd;
    int connected;
};

struct TcpServer {
    int fd;
    int port;
};

static int net_initialized = 0;

void aether_net_init(void) {
    if (net_initialized) return;
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    net_initialized = 1;
}

/* ---- Dialling (see aether_net_addr.h) ---------------------------------- */

typedef struct {
    char host[128];
    int  port;
    int  family;
} NetFamilyMemo;

#define NET_FAMILY_MEMO_SLOTS 32
static NetFamilyMemo net_family_memo[NET_FAMILY_MEMO_SLOTS];
static int net_family_memo_next = 0;
/* CRITICAL_SECTION, which the thread shim maps pthread_mutex_t to on Windows,
 * has no static initialiser, so the lock is created once through an atomic
 * gate there (the same shape as the HTTP pool's in aether_http.c). */
#if defined(_WIN32)
#include <stdatomic.h>
static pthread_mutex_t net_family_memo_mutex;
static atomic_int      net_family_memo_state = 0;   /* 0 unset, 1 setting, 2 ready */
static pthread_mutex_t* net_family_memo_lock(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&net_family_memo_state, &expected, 1)) {
        pthread_mutex_init(&net_family_memo_mutex, NULL);
        atomic_store(&net_family_memo_state, 2);
    } else {
        while (atomic_load(&net_family_memo_state) != 2) { /* one-shot spin */ }
    }
    return &net_family_memo_mutex;
}
#else
static pthread_mutex_t net_family_memo_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* net_family_memo_lock(void) { return &net_family_memo_mutex; }
#endif

static int net_family_lookup(const char* host, int port) {
    int family = 0;
    if (!host || strlen(host) >= sizeof net_family_memo[0].host) return 0;
    pthread_mutex_lock(net_family_memo_lock());
    for (int i = 0; i < NET_FAMILY_MEMO_SLOTS; i++) {
        if (net_family_memo[i].family && net_family_memo[i].port == port &&
            strcmp(net_family_memo[i].host, host) == 0) {
            family = net_family_memo[i].family;
            break;
        }
    }
    pthread_mutex_unlock(net_family_memo_lock());
    return family;
}

void aether_net_note_family(const char* host, int port, int family) {
    if (!host || strlen(host) >= sizeof net_family_memo[0].host) return;
    pthread_mutex_lock(net_family_memo_lock());
    int slot = -1;
    for (int i = 0; i < NET_FAMILY_MEMO_SLOTS; i++) {
        if (net_family_memo[i].family && net_family_memo[i].port == port &&
            strcmp(net_family_memo[i].host, host) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = net_family_memo_next;
        net_family_memo_next = (net_family_memo_next + 1) % NET_FAMILY_MEMO_SLOTS;
        strcpy(net_family_memo[slot].host, host);
        net_family_memo[slot].port = port;
    }
    net_family_memo[slot].family = family;
    pthread_mutex_unlock(net_family_memo_lock());
}

int aether_net_order(const char* host, int port, AetherAddrList* addrs) {
    int family = net_family_lookup(host, port);
    if (!family || addrs->n < 2) return family;
    /* A stable partition: the remembered family first, each side in
     * getaddrinfo's order. */
    AetherAddrList sorted;
    sorted.n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < addrs->n; i++) {
            int match = addrs->ss[i].ss_family == family;
            if (match == (pass == 0)) {
                sorted.ss[sorted.n] = addrs->ss[i];
                sorted.len[sorted.n] = addrs->len[i];
                sorted.n++;
            }
        }
    }
    *addrs = sorted;
    return family;
}

int aether_net_connect_addr(const struct sockaddr_storage* addr, socklen_t len,
                            long long timeout_ns, int read_buffer,
                            const char** why, int* timed_out) {
    *timed_out = 0;
    int sockfd = (int)socket(addr->ss_family, SOCK_STREAM, 0);
    if (sockfd < 0) {
        *why = "could not create socket";
        return -1;
    }
    /* Before connect, so the SYN advertises a window this size (#3338). Set
     * on a connected socket, the buffer shrinks, but the kernel still takes
     * the window it already advertised, and on macOS it went on taking
     * megabytes from a sender the reader had stopped reading. */
    if (read_buffer > 0 &&
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const char*)&read_buffer,
                   sizeof(read_buffer)) != 0) {
        close(sockfd);
        *why = "could not set the receive buffer";
        return -1;
    }
    if (timeout_ns <= 0) {
        if (connect(sockfd, (const struct sockaddr*)addr, len) < 0) {
            close(sockfd);
            *why = "connection failed";
            return -1;
        }
        return sockfd;
    }

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(sockfd, FIONBIO, &nb);
#else
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
#endif
    if (connect(sockfd, (const struct sockaddr*)addr, len) < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        int in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        int in_progress = (errno == EINPROGRESS || errno == EWOULDBLOCK);
#endif
        if (!in_progress) {
            close(sockfd);
            *why = "connection failed";
            return -1;
        }
        /* Watch both the writable set (success) and the exception set
         * (failure). On POSIX, a refused non-blocking connect makes the
         * socket writable with SO_ERROR=ECONNREFUSED. On Windows, Winsock
         * signals connect failures via the *exception* fd set instead, NOT
         * writable -- so a select that watches only wfds waits the full
         * timeout for refused connects rather than failing fast. SO_ERROR
         * distinguishes success from failure afterwards. */
#ifdef _WIN32
        /* Winsock's fd_set is a small array of SOCKET handles, not a bitmap
         * indexed by descriptor, so it has no FD_SETSIZE hazard for a single
         * socket. */
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_SET(sockfd, &wfds);
        FD_ZERO(&efds); FD_SET(sockfd, &efds);
        struct timeval tv;
        tv.tv_sec  = (long)(timeout_ns / 1000000000LL);
        tv.tv_usec = (long)((timeout_ns / 1000LL) % 1000000LL);
        int sel = select(sockfd + 1, NULL, &wfds, &efds, &tv);
#else
        /* poll, not select: a POSIX fd_set is a bitmap of FD_SETSIZE bits,
         * and FD_SET on a descriptor at or above it writes past the end. A
         * process serving many connections (a proxy, which is what dials
         * most often) passes 1024 descriptors easily. POLLOUT covers both
         * outcomes: a refused non-blocking connect reports the socket ready
         * with SO_ERROR set, which the check below reads. */
        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        long long timeout_ms = (timeout_ns + 999999LL) / 1000000LL;
        if (timeout_ms > INT_MAX) timeout_ms = INT_MAX;
        int sel = poll(&pfd, 1, (int)timeout_ms);
#endif
        if (sel == 0) {
            close(sockfd);
            *why = "connect timeout";
            *timed_out = 1;
            return -1;
        }
        if (sel < 0) {
            close(sockfd);
            *why = "select on connect failed";
            return -1;
        }
        int so_err = 0;
        socklen_t slen = sizeof(so_err);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char*)&so_err, &slen) < 0
            || so_err != 0) {
            close(sockfd);
            *why = "connection failed";
            return -1;
        }
    }
    /* Back to blocking for the send/recv path, which uses SO_*TIMEO. */
#ifdef _WIN32
    nb = 0;
    ioctlsocket(sockfd, FIONBIO, &nb);
#else
    if (flags >= 0) fcntl(sockfd, F_SETFL, flags);
#endif
    return sockfd;
}

int aether_net_dial_addrs(const char* host, int port, AetherAddrList* addrs,
                          long long timeout_ns, int read_buffer, const char** why) {
    *why = "could not resolve host";
    if (!addrs || addrs->n <= 0) return -1;
    /* A family that has connected to this host and port before is a proven
     * route: its addresses get the whole budget, as a single address would.
     * Only an unproven address is held to the short first try -- cutting a
     * proven one short turns a busy server's dropped SYN (retried after a
     * second) into a failed dial. */
    int proven = aether_net_order(host, port, addrs);

    const long long first_try = (long long)AETHER_DIAL_FIRST_TRY_MS * 1000000LL;
    unsigned long long deadline = timeout_ns > 0
        ? aether_now_ns() + (unsigned long long)timeout_ns : 0;
    int again[AETHER_ADDR_MAX];
    int n_again = 0;

    for (int pass = 0; pass < 2; pass++) {
        int count = pass == 0 ? addrs->n : n_again;
        for (int k = 0; k < count; k++) {
            int i = pass == 0 ? k : again[k];
            int last = pass == 1 || k == count - 1;
            long long left = 0;   /* 0: no limit */
            if (deadline) {
                unsigned long long now = aether_now_ns();
                if (now >= deadline) { *why = "connect timeout"; return -1; }
                left = (long long)(deadline - now);
            }
            long long t = left;
            if (!last && addrs->ss[i].ss_family != proven)
                t = (left > 0 && left < first_try) ? left : first_try;
            int timed_out = 0;
            int fd = aether_net_connect_addr(&addrs->ss[i], addrs->len[i], t, read_buffer,
                                             why, &timed_out);
            if (fd >= 0) {
                if (addrs->n > 1) aether_net_note_family(host, port, addrs->ss[i].ss_family);
                return fd;
            }
            if (pass == 0 && !last && timed_out && addrs->ss[i].ss_family != proven)
                again[n_again++] = i;
        }
    }
    return -1;
}

int aether_net_dial(const char* host, int port, long long timeout_ns, const char** why) {
    AetherAddrList addrs;
    if (aether_addr_resolve(host, port, &addrs) != 0) {
        *why = "could not resolve host";
        return -1;
    }
    return aether_net_dial_addrs(host, port, &addrs, timeout_ns, 0, why);
}

// Set read/write timeouts on a socket so blocking operations don't hang
// indefinitely on dead or slow peers.  Default: 30 seconds.
static void net_set_socket_timeouts(int fd, int timeout_sec) {
#ifdef _WIN32
    DWORD timeout_ms = (DWORD)timeout_sec * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

/* tcp_connect_raw, and with `read_buffer` > 0 the receive buffer set before
 * the connection is made (tcp_connect_buffered_raw). */
static TcpSocket* tcp_connect_with(const char* host, int port, int read_buffer) {
    // Sandbox check: is TCP connect to this host and port allowed?
    char tcp_res[300];
    if (!aether_sandbox_check("tcp", aether_net_resource(tcp_res, sizeof tcp_res, host, port))) return NULL;

    aether_net_init();

    /* getaddrinfo, not gethostbyname: the latter returns a pointer into a
     * shared, process-static struct, so concurrent connects on different
     * threads race and corrupt each other's resolved address. getaddrinfo is
     * thread-safe and returns caller-owned memory. Each address the host has,
     * IPv6 or IPv4, is tried as aether_net_dial_addrs describes. */
    const char* why = NULL;
    AetherAddrList addrs;
    if (aether_addr_resolve(host, port, &addrs) != 0) return NULL;
    int sockfd = aether_net_dial_addrs(host, port, &addrs, 0, read_buffer, &why);
    if (sockfd < 0) return NULL;

    net_set_socket_timeouts(sockfd, 30);

    TcpSocket* sock = (TcpSocket*)malloc(sizeof(TcpSocket));
    if (!sock) { close(sockfd); return NULL; }
    sock->fd = sockfd;
    sock->connected = 1;
    return sock;
}

TcpSocket* tcp_connect_raw(const char* host, int port) {
    return tcp_connect_with(host, port, 0);
}

TcpSocket* tcp_connect_buffered_raw(const char* host, int port, int read_buffer) {
    if (read_buffer <= 0) return NULL;
    return tcp_connect_with(host, port, read_buffer);
}

int tcp_send_raw(TcpSocket* sock, const char* data) {
    if (!sock || !sock->connected || !data) return -1;

    int sent = send(sock->fd, data, strlen(data), 0);
    return sent;
}

int tcp_send_n_raw(TcpSocket* sock, const char* data, int length) {
    if (!sock || !sock->connected || !data || length < 0) return -1;
    if (length == 0) return 0;

    int sent = send(sock->fd, data, (size_t)length, 0);
    return sent;
}

char* tcp_receive_raw(TcpSocket* sock, int max_bytes) {
    if (!sock || !sock->connected || max_bytes <= 0) return NULL;

    /* Cap-aware (#343): max_bytes is caller-supplied (program-
     * controlled, but plugin-host paths surface it as untrusted).
     * The buffer becomes the payload of the counted string returned. */
    char* buffer = (char*)aether_caps_malloc((size_t)max_bytes + 1);
    if (!buffer) return NULL;
    int received = recv(sock->fd, buffer, max_bytes, 0);

    if (received <= 0) {
        aether_caps_free(buffer, (size_t)max_bytes + 1);
        /* Idle peer (recv-timeout / would-block) is not a close: leave the
         * socket connected so the caller can retry (#1092). Only a genuine
         * FIN (received == 0) or hard error marks it dead. This text path
         * still collapses both to NULL — callers needing to tell idle from
         * closed must use tcp_receive_n_raw, whose "timeout" sentinel is
         * distinct. */
        if (!(received < 0 && aether_net_wouldblock())) {
            sock->connected = 0;
        }
        return NULL;
    }

    buffer[received] = '\0';
    return (char*)string_adopt_caps_buffer(buffer, (size_t)received,
                                           (size_t)max_bytes + 1);
}

TcpReceiveResult tcp_receive_n_raw(TcpSocket* sock, int max_bytes) {
    TcpReceiveResult out;
    out._0 = NULL;
    out._1 = 0;
    out._2 = "connection closed or receive failed";

    if (!sock || !sock->connected || max_bytes <= 0) {
        return out;
    }

    char* buffer = (char*)aether_caps_malloc((size_t)max_bytes);
    if (!buffer) {
        out._2 = "allocation failed";
        return out;
    }

    int received = recv(sock->fd, buffer, max_bytes, 0);
    if (received <= 0) {
        aether_caps_free(buffer, (size_t)max_bytes);
        /* Split the failure branch (#1092): a would-block / recv-timeout means
         * "peer idle, try again" — return the distinct "timeout" sentinel and
         * KEEP the socket connected. Only an orderly FIN (received == 0) or a
         * hard error is a real close that marks the socket dead. Collapsing
         * these (the old behaviour) tore down full-duplex tunnels the first
         * time either direction went quiet for 30 s. */
        if (received < 0 && aether_net_wouldblock()) {
            out._2 = "timeout";
        } else {
            sock->connected = 0;
        }
        return out;
    }

    AetherString* wrapped = string_new_with_length(buffer, (size_t)received);
    aether_caps_free(buffer, (size_t)max_bytes);
    if (!wrapped) {
        out._2 = "allocation failed";
        return out;
    }

    out._0 = (void*)wrapped;
    out._1 = received;
    out._2 = "";
    return out;
}

/* Byte-buffer twin of tcp_send_n_raw for std.tcp's slice wrapper (#2301):
 * the data is a `void*` the caller has already bounded, never a string.
 * Returns the send(2) count, which may be short, or -1. */
int tcp_send_bytes_raw(TcpSocket* sock, const void* data, int length) {
    if (!sock || !sock->connected || length < 0) return -1;
    if (length == 0) return 0;
    if (!data) return -1;
    int sent = send(sock->fd, (const char*)data, (size_t)length, 0);
    return sent < 0 ? -1 : sent;
}

#ifndef _WIN32
/* Where tcp_send_all_raw is in its send timeout: the socket's SO_SNDTIMEO
 * (-1 until read, 0 for none), when the current period of it ends (0 before
 * the first wait), and whether any byte went in that period. */
typedef struct {
    long long timeout_ms;
    long long due_ms;
    int       sent;
} NetSendWait;

static long long net_now_ms(void) {
    return (long long)(aether_now_ns() / 1000000);
}

/* Wait for `fd` to take more bytes: 1 when it can, else 0. The send timeout
 * keeps the meaning a blocking send gives it (#3108): a period of it in
 * which some bytes went is followed by another, and one in which none went
 * ends the send. What changes is the wake: poll(2) reports the socket
 * writable once a good part of its buffer is free, not for every byte that
 * frees (#3257). For a blocking socket only: one its owner made non-blocking
 * asked not to be waited on. */
static int net_wait_writable(int fd, NetSendWait* w) {
    if (w->timeout_ms < 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || (flags & O_NONBLOCK)) return 0;
        struct timeval tv = { 0, 0 };
        socklen_t tl = sizeof tv;
        w->timeout_ms = getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, &tl) == 0
                      ? (long long)tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000 : 0;
    }
    if (w->timeout_ms > 0 && !w->due_ms) w->due_ms = net_now_ms() + w->timeout_ms;
    for (;;) {
        int wait = -1;
        if (w->timeout_ms > 0) {
            long long left = w->due_ms - net_now_ms();
            if (left <= 0) {
                if (!w->sent) return 0;            /* a whole period, nothing went */
                w->sent = 0;
                w->due_ms = net_now_ms() + w->timeout_ms;
                left = w->timeout_ms;
            }
            wait = left > INT_MAX ? INT_MAX : (int)left;
        }
        struct pollfd pfd = { fd, POLLOUT, 0 };
        int r = poll(&pfd, 1, wait);
        if (r > 0) return 1;      /* an error or a hangup: the send reports it */
        if (r < 0 && errno != EINTR) return 0;
    }
}
#endif

/* #3108: every one of `length` bytes, as many send(2) calls as it takes:
 * one can take fewer than asked (the send timeout expiring while the peer
 * reads slowly, a signal). An interrupted send is retried; the first that
 * fails, or times out having sent nothing, ends it. Returns the bytes sent:
 * `length` once every one is out, fewer when a send failed.
 *
 * On POSIX each send goes without blocking and a full socket is waited on in
 * net_wait_writable, which keeps that meaning of the send timeout (#3257). A
 * blocking send wakes for any room that frees: on Linux a peer that read
 * nothing for three seconds kept a send under a one second timeout going all
 * that time. The pure-Aether TLS server writes through here. */
int tcp_send_all_raw(TcpSocket* sock, const void* data, int length) {
    if (!sock || !sock->connected || length <= 0 || !data) return 0;
    const char* p = (const char*)data;
    int sent = 0;
#ifndef _WIN32
    NetSendWait wait = { -1, 0, 0 };
#endif
    while (sent < length) {
#ifdef _WIN32
        int n = send(sock->fd, p + sent, (size_t)(length - sent), 0);
        if (n > 0) {
            sent += n;
            continue;
        }
        if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
#else
        int n = (int)send(sock->fd, p + sent, (size_t)(length - sent), MSG_DONTWAIT);
        if (n > 0) {
            sent += n;
            wait.sent = 1;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            net_wait_writable(sock->fd, &wait))
            continue;
#endif
        break;
    }
    return sent;
}

/* Receive into a caller-owned buffer of `cap` bytes (#2301): one recv(2)
 * of at most `cap` bytes, nothing allocated. Returns the count (> 0),
 * TCP_RECV_TIMEOUT for an idle peer (recv-timeout / would-block: the
 * socket stays connected, as in tcp_receive_n_raw, #1092), or
 * TCP_RECV_CLOSED for an orderly FIN, a hard error (both mark the socket
 * dead), a null/closed handle or a bad buffer. */
int tcp_receive_into_raw(TcpSocket* sock, void* buf, int cap) {
    if (!sock || !sock->connected || !buf || cap <= 0) return TCP_RECV_CLOSED;
    int received = recv(sock->fd, (char*)buf, cap, 0);
    if (received > 0) return received;
    if (received < 0 && aether_net_wouldblock()) return TCP_RECV_TIMEOUT;
    sock->connected = 0;
    return TCP_RECV_CLOSED;
}

int tcp_close(TcpSocket* sock) {
    if (!sock) return -1;

    /* A peer EOF/error clears connected, but the handle still owns fd. */
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
        sock->connected = 0;
    }

    free(sock);
    return 0;
}

/* One listener for both entry points (#2136). `address` NULL means every
 * IPv4 interface (INADDR_ANY, what tcp_listen_raw always did), "::" every
 * interface of both families; a numeric IPv4 or IPv6 address binds that
 * interface alone — "127.0.0.1" for a debugging
 * channel that must not be reachable from the network. `allow_zero`
 * lets port 0 through, asking the OS for an ephemeral port that is read
 * back with getsockname so tcp_server_port_raw can report it. */
static TcpServer* tcp_listen_impl(const char* address, int port, int allow_zero) {
    // Sandbox check: is listening on this port allowed?
    if (!aether_sandbox_check("tcp_listen", "*")) return NULL;

    aether_net_init();

    if (port < (allow_zero ? 0 : 1) || port > 65535) {
        return NULL;
    }

    /* An address is a numeric one, IPv4 or IPv6 (bracketed or not); a name
     * is refused rather than resolved. "::" is every interface of both
     * families. */
    if (address && address[0]) {
        char bare[64];
        unsigned char probe[16];
        if (!aether_addr_unbracket(address, bare, sizeof bare) ||
            (inet_pton(AF_INET, bare, probe) != 1 && inet_pton(AF_INET6, bare, probe) != 1)) {
            return NULL;   /* not a numeric address */
        }
    }
    struct sockaddr_storage serv_addr;
    socklen_t serv_len = 0;
    if (aether_addr_bind(address, port, &serv_addr, &serv_len) != 0) {
        return NULL;
    }

    int sockfd = aether_addr_listen_socket(&serv_addr);
    if (sockfd < 0) {
        return NULL;
    }

    if (bind(sockfd, (struct sockaddr*)&serv_addr, serv_len) < 0) {
        close(sockfd);
        return NULL;
    }

    if (listen(sockfd, 5) < 0) {
        close(sockfd);
        return NULL;
    }

    int bound = port;
    if (port == 0) {
        struct sockaddr_storage got;
        socklen_t got_len = sizeof(got);
        memset(&got, 0, sizeof(got));
        if (getsockname(sockfd, (struct sockaddr*)&got, &got_len) == 0) {
            bound = aether_addr_port(&got);
        }
    }

    TcpServer* server = (TcpServer*)malloc(sizeof(TcpServer));
    if (!server) { close(sockfd); return NULL; }
    server->fd = sockfd;
    server->port = bound;
    return server;
}

TcpServer* tcp_listen_raw(int port) {
    return tcp_listen_impl(NULL, port, 0);
}

TcpServer* tcp_listen_on_raw(const char* address, int port) {
    return tcp_listen_impl(address, port, 1);
}

int tcp_server_port_raw(TcpServer* server) {
    return server ? server->port : -1;
}


int tcp_set_nodelay_raw(TcpSocket* sock, int on) {
    if (!sock || !sock->connected) return -1;
    int flag = on ? 1 : 0;
    if (setsockopt(sock->fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag)) != 0) {
        return -1;
    }
    return 0;
}

/* SO_RCVBUF: the socket's receive buffer, `bytes` as the kernel takes it
 * (Linux doubles it for its bookkeeping). Setting it also stops the kernel
 * growing the buffer by itself, as Linux, macOS and Windows otherwise do
 * while data arrives, read or not. */
int tcp_set_read_buffer_raw(TcpSocket* sock, int bytes) {
    if (!sock || !sock->connected || bytes <= 0) return -1;
    /* DIAGNOSTIC ONLY (#3338, never merged). */
    int before = 0, after = 0;
    socklen_t bl = sizeof(before);
    getsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, (char*)&before, &bl);
    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    socklen_t al = sizeof(after);
    getsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, (char*)&after, &al);
    if (getenv("AETHER_SENDWAIT_LOG"))
        fprintf(stderr, "[rcvbuf] fd=%d before=%d asked=%d after=%d\n", sock->fd, before, bytes, after);
    return 0;
}

TcpSocket* tcp_accept_raw(TcpServer* server) {
    if (!server) return NULL;

    struct sockaddr_storage cli_addr;
    socklen_t clilen = sizeof(cli_addr);

    int newsockfd = accept(server->fd, (struct sockaddr*)&cli_addr, &clilen);
    if (newsockfd < 0) {
        return NULL;
    }

    net_set_socket_timeouts(newsockfd, 30);

    TcpSocket* sock = (TcpSocket*)malloc(sizeof(TcpSocket));
    if (!sock) { close(newsockfd); return NULL; }
    sock->fd = newsockfd;
    sock->connected = 1;
    return sock;
}

int tcp_server_close(TcpServer* server) {
    if (!server) return -1;

    close(server->fd);
    free(server);
    return 0;
}

/* Expose the OS-level descriptors inside the opaque socket handles
 * (issue #1003). Exists so capability plumbing — capsicum.rights_limit()
 * / fcntls_limit() before capsicum.enter() — can narrow a socket the
 * program opened through the stdlib. The fd is owned by the handle:
 * callers must not close() it; it dies with tcp_close /
 * tcp_server_close. Returns -1 on a null/closed handle. */
int tcp_fd_raw(TcpSocket* sock) {
    if (!sock || !sock->connected) return -1;
    return sock->fd;
}

int tcp_server_fd_raw(TcpServer* server) {
    if (!server) return -1;
    return server->fd;
}

TcpSocket* tcp_socket_from_fd_owned(int fd) {
    if (fd < 0) return NULL;
    aether_net_init();
#ifndef _WIN32
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
    net_set_socket_timeouts(fd, 30);

    TcpSocket* sock = (TcpSocket*)malloc(sizeof(TcpSocket));
    if (!sock) {
        close(fd);
        return NULL;
    }
    sock->fd = fd;
    sock->connected = 1;
    return sock;
}

/* Readiness primitives (#1092). A full-duplex relay must wait on both
 * directions at once and service whichever becomes readable; blocking
 * read_n from one thread of control cannot express that. These wrap
 * poll(2) directly — the smallest surface that unblocks a CONNECT-tunnel
 * byte pump without entangling std.tcp with the scheduler's actor poller.
 *
 * timeout_ms: -1 blocks indefinitely, 0 polls, >0 waits that many ms.
 * On Windows, WSAPoll has the same shape for connected TCP sockets. */
#ifdef _WIN32
    #define AE_POLLFD  WSAPOLLFD
    #define ae_poll    WSAPoll
    #define ae_nfds_t  ULONG
#else
    #define AE_POLLFD  struct pollfd
    #define ae_poll    poll
    #define ae_nfds_t  nfds_t
#endif

/* Wait until a single socket is readable. Returns 1 if readable (data or
 * EOF pending — a following recv distinguishes them), 0 on timeout, -1 on
 * a null/closed handle or poll error. Does NOT read and does NOT alter the
 * socket's connected flag. */
int tcp_server_poll_raw(TcpServer* server, int timeout_ms) {
    if (!server) return -1;
    AE_POLLFD pfd;
    pfd.fd = server->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = ae_poll(&pfd, (ae_nfds_t)1, timeout_ms);
    if (rc < 0) {
        if (aether_net_wouldblock()) return 0;   /* EINTR: treat as no-event */
        return -1;
    }
    if (rc == 0) return 0;
    return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
}

int tcp_poll_raw(TcpSocket* sock, int timeout_ms) {
    if (!sock || !sock->connected) return -1;
    AE_POLLFD pfd;
    pfd.fd = sock->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = ae_poll(&pfd, (ae_nfds_t)1, timeout_ms);
    if (rc < 0) {
        if (aether_net_wouldblock()) return 0;   /* EINTR: treat as no-event */
        return -1;
    }
    if (rc == 0) return 0;                      /* timeout */
    return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
}

/* Wait until either of two sockets is readable — the relay primitive.
 * Returns a bitmask: bit 0 (1) = `a` readable, bit 1 (2) = `b` readable
 * (both bits may be set). 0 on timeout. -1 if both handles are
 * null/closed or on a poll error. A null/closed handle on one side is
 * simply not watched, so a half-open relay still waits on the live side. */
int tcp_poll2_raw(TcpSocket* a, TcpSocket* b, int timeout_ms) {
    AE_POLLFD pfds[2];
    int idx_a = -1, idx_b = -1;
    ae_nfds_t n = 0;
    if (a && a->connected) {
        pfds[n].fd = a->fd; pfds[n].events = POLLIN; pfds[n].revents = 0;
        idx_a = (int)n; n++;
    }
    if (b && b->connected) {
        pfds[n].fd = b->fd; pfds[n].events = POLLIN; pfds[n].revents = 0;
        idx_b = (int)n; n++;
    }
    if (n == 0) return -1;

    int rc = ae_poll(pfds, n, timeout_ms);
    if (rc < 0) {
        if (aether_net_wouldblock()) return 0;   /* EINTR: caller re-polls */
        return -1;
    }
    if (rc == 0) return 0;                      /* timeout */

    int ready = 0;
    const short mask = POLLIN | POLLHUP | POLLERR;
    if (idx_a >= 0 && (pfds[idx_a].revents & mask)) ready |= 1;
    if (idx_b >= 0 && (pfds[idx_b].revents & mask)) ready |= 2;
    return ready;
}

#endif // AETHER_HAS_NETWORKING
