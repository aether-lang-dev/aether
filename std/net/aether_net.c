#include "aether_net.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"
#include "../string/aether_string.h"

#if !AETHER_HAS_NETWORKING
TcpSocket* tcp_connect_raw(const char* h, int p) { (void)h; (void)p; return NULL; }
TcpSocket* tcp_connect_buffered_raw(const char* h, int p, int b) { (void)h; (void)p; (void)b; return NULL; }
TcpOpenResult tcp_connect_timeout_raw(const char* h, int p, int t) {
    (void)h; (void)p; (void)t;
    TcpOpenResult out = { NULL, "networking disabled at build time" };
    return out;
}
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
TcpOpenResult tcp_connect_unix_raw(const char* p) {
    (void)p;
    TcpOpenResult out = { NULL, "networking disabled at build time" };
    return out;
}
TcpOpenResult tcp_listen_unix_raw(const char* p) {
    (void)p;
    TcpOpenResult out = { NULL, "networking disabled at build time" };
    return out;
}
int aether_net_dial_unix(const char* p, const char** why) { (void)p; *why = "networking disabled at build time"; return -1; }
TcpServer* tcp_listen_raw(int p) { (void)p; return NULL; }
TcpServer* tcp_listen_on_raw(const char* a, int p) { (void)a; (void)p; return NULL; }
int tcp_server_port_raw(TcpServer* s) { (void)s; return -1; }
int tcp_server_poll_raw(TcpServer* s, int t) { (void)s; (void)t; return -1; }
int tcp_set_nodelay_raw(TcpSocket* s, int on) { (void)s; (void)on; return -1; }
int tcp_set_read_buffer_raw(TcpSocket* s, int b) { (void)s; (void)b; return -1; }
int tcp_available_raw(TcpSocket* s) { (void)s; return -1; }
TcpSocket* tcp_accept_raw(TcpServer* s) { (void)s; return NULL; }
int tcp_server_close(TcpServer* s) { (void)s; return 0; }
int tcp_fd_raw(TcpSocket* s) { (void)s; return -1; }
int tcp_server_fd_raw(TcpServer* s) { (void)s; return -1; }
TcpSocket* tcp_socket_from_fd_owned(int fd) { (void)fd; return NULL; }
TcpSocket* tcp_socket_adopt_fd(int fd) { (void)fd; return NULL; }
int tcp_poll_raw(TcpSocket* s, int t) { (void)s; (void)t; return -1; }
int tcp_poll2_raw(TcpSocket* a, TcpSocket* b, int t) { (void)a; (void)b; (void)t; return -1; }
int tcp_wait_writable_raw(TcpSocket* s, int t) { (void)s; (void)t; return -1; }
int tcp_send_some_raw(TcpSocket* s, const void* d, int n) { (void)s; (void)d; (void)n; return -1; }
TcpOpenResult tcp_connect_until_raw(const char* h, int p, long long d, TcpCancel* c) {
    (void)h; (void)p; (void)d; (void)c;
    TcpOpenResult out = { NULL, "networking disabled at build time" };
    return out;
}
int tcp_set_deadline_raw(TcpSocket* s, long long d, TcpCancel* c) { (void)s; (void)d; (void)c; return -1; }
long long tcp_deadline_raw(TcpSocket* s) { (void)s; return 0; }
int tcp_stop_raw(TcpSocket* s) { (void)s; return TCP_STOP_NONE; }
TcpOpenResult tcp_cancel_new_raw(void) {
    TcpOpenResult out = { NULL, "networking disabled at build time" };
    return out;
}
int tcp_cancel_raw(TcpCancel* c) { (void)c; return -1; }
int tcp_cancel_tripped_raw(TcpCancel* c) { (void)c; return 0; }
int tcp_cancel_retain_raw(TcpCancel* c) { (void)c; return -1; }
void tcp_cancel_free_raw(TcpCancel* c) { (void)c; }
void aether_net_init(void) {}
int aether_net_wouldblock(void) { return 0; }
#else

#include <stdlib.h>
#include <string.h>
#include <stdio.h>   /* snprintf (getaddrinfo port string) */
#include <errno.h>   /* EAGAIN/EWOULDBLOCK: idle-peer vs. closed-peer recv */
#include <stddef.h>  /* offsetof (sockaddr_un length) */
#include <stdint.h>
#include <stdatomic.h> /* TcpCancel: its flag and count, read by any thread */

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define close closesocket
    typedef int socklen_t;
    /* AF_UNIX on Windows 10 1803 and later. Older MinGW headers lack
     * afunix.h; the struct is the one it declares. */
    #if defined(__has_include)
        #if __has_include(<afunix.h>)
            #include <afunix.h>
            #define AE_HAVE_AFUNIX_H 1
        #endif
    #endif
    #ifndef AE_HAVE_AFUNIX_H
        #define UNIX_PATH_MAX 108
        struct sockaddr_un {
            ADDRESS_FAMILY sun_family;
            char sun_path[UNIX_PATH_MAX];
        };
    #endif
#else
    #include <sys/un.h>     /* sockaddr_un: Unix-domain stream sockets */
    #include <sys/socket.h>
    #include <sys/time.h>   /* struct timeval: glibc leaks it via other headers, musl does not */
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <netinet/tcp.h>   /* TCP_NODELAY (#2136) */
    #include <sys/ioctl.h>     /* FIONREAD (tcp_available_raw) */
    #include <poll.h>
    #include <fcntl.h>
    #if defined(__linux__)
        #include <sys/eventfd.h>   /* the cancel handle's wake (TcpCancel) */
    #endif
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

/* recv(2) on a std.tcp socket, taken again when a signal interrupts it
 * before any byte came: an interruption is not the peer going quiet, so
 * the "timeout" a receive reports means the socket's receive timeout (or a
 * non-blocking socket with nothing buffered), which a caller may take as
 * the timeout it set (#3703). Windows has no EINTR. */
static int net_recv(int fd, void* buf, int len) {
    int received;
    do {
        received = (int)recv(fd, (char*)buf, len, 0);
#ifndef _WIN32
    } while (received < 0 && errno == EINTR);
#else
    } while (0);
#endif
    return received;
}

struct TcpSocket {
    int fd;
    int connected;
    /* The deadline and cancel bound to this connection (tcp_set_deadline_raw,
     * tcp_connect_until_raw): due_ms on aether_now_ns's monotonic clock in
     * ms, 0 for none; cancel a counted reference, NULL for none. stop says
     * which of them ended the last operation stopped by one (TCP_STOP_*). */
    long long due_ms;
    TcpCancel* cancel;
    int stop;
};

/* A cancel handle (tcp_cancel_new_raw): a flag, and a descriptor that turns
 * readable when the flag is set, which every wait of a socket bound to it
 * polls beside the socket. Tripping it writes to its own descriptor only;
 * the canceller never touches a connection's. Counted: the creator holds
 * one reference (tcp_cancel_free_raw drops it) and each bound socket one
 * (tcp_close drops it), so its descriptors outlive every wait on them.
 *   Linux: an eventfd (rd == wr).
 *   Other POSIX (macOS, iOS, the BSDs): a pipe, both ends non-blocking.
 *   Windows: a connected loopback TCP pair, since select() and WSAPoll wait
 *   on sockets alone; the accepted end is checked to be the one this
 *   process connected. */
struct TcpCancel {
    _Atomic int tripped;
    _Atomic int refs;
    int rd;
    int wr;
};

/* A std.tcp handle for the connected descriptor `fd`, which it then owns:
 * NULL (and `fd` closed) when there is no memory. */
static TcpSocket* net_sock_new(int fd) {
    TcpSocket* sock = (TcpSocket*)malloc(sizeof(TcpSocket));
    if (!sock) { close(fd); return NULL; }
    sock->fd = fd;
    sock->connected = 1;
    sock->due_ms = 0;
    sock->cancel = NULL;
    sock->stop = TCP_STOP_NONE;
    return sock;
}

/* ---- Deadlines and cancellation ---------------------------------------- */

/* Monotonic milliseconds: aether_now_ns's clock, which is std.os's
 * now_monotonic_ms (CLOCK_MONOTONIC; QueryPerformanceCounter on Windows). */
static long long net_mono_ms(void) {
    return (long long)(aether_now_ns() / 1000000ULL);
}

static void net_cancel_ref(TcpCancel* c) {
    atomic_fetch_add(&c->refs, 1);
}

static void net_cancel_unref(TcpCancel* c) {
    if (atomic_fetch_sub(&c->refs, 1) != 1) return;
#if defined(__linux__)
    close(c->rd);
#else
    close(c->rd);
    close(c->wr);
#endif
    free(c);
}

#ifdef _WIN32
/* A connected loopback pair for the wake: rd is the accepted end, wr the
 * connecting one. Another local process could connect to the listener in
 * the moment it is open, so the accepted end's peer must be wr's own
 * address. 0, or -1 with nothing left open. */
static int net_wake_pair(int* rd, int* wr) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int alen = (int)sizeof a;
    SOCKET c = INVALID_SOCKET, s = INVALID_SOCKET;
    if (bind(l, (struct sockaddr*)&a, alen) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr*)&a, &alen) != 0) goto fail;
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET || connect(c, (struct sockaddr*)&a, alen) != 0) goto fail;
    s = accept(l, NULL, NULL);
    if (s == INVALID_SOCKET) goto fail;
    struct sockaddr_in mine, theirs;
    int ml = (int)sizeof mine, tl = (int)sizeof theirs;
    if (getsockname(c, (struct sockaddr*)&mine, &ml) != 0 ||
        getpeername(s, (struct sockaddr*)&theirs, &tl) != 0 ||
        mine.sin_port != theirs.sin_port ||
        mine.sin_addr.s_addr != theirs.sin_addr.s_addr) goto fail;
    closesocket(l);
    *rd = (int)s;
    *wr = (int)c;
    return 0;
fail:
    if (s != INVALID_SOCKET) closesocket(s);
    if (c != INVALID_SOCKET) closesocket(c);
    closesocket(l);
    return -1;
}
#endif

TcpOpenResult tcp_cancel_new_raw(void) {
    TcpOpenResult out = { NULL, "could not make the cancel handle" };
    aether_net_init();
    TcpCancel* c = (TcpCancel*)malloc(sizeof(TcpCancel));
    if (!c) { out._1 = "out of memory"; return out; }
    atomic_init(&c->tripped, 0);
    atomic_init(&c->refs, 1);
#if defined(_WIN32)
    if (net_wake_pair(&c->rd, &c->wr) != 0) { free(c); return out; }
#elif defined(__linux__)
    int e = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (e < 0) { free(c); return out; }
    c->rd = c->wr = e;
#else
    int p[2];
    if (pipe(p) != 0) { free(c); return out; }
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(p[i], F_GETFL, 0);
        if (fl >= 0) (void)fcntl(p[i], F_SETFL, fl | O_NONBLOCK);
        (void)fcntl(p[i], F_SETFD, FD_CLOEXEC);
    }
    c->rd = p[0];
    c->wr = p[1];
#endif
    out._0 = c;
    out._1 = "";
    return out;
}

/* Trip `c`: from any thread, any number of times; only the first writes,
 * one byte (eight on an eventfd) to the handle's own descriptor, which is
 * never read, so it stays readable. On POSIX this is an atomic exchange
 * and one write(2), both async-signal-safe, so a signal handler may call
 * it. 0, or -1 for a null handle. */
int tcp_cancel_raw(TcpCancel* c) {
    if (!c) return -1;
    if (atomic_exchange(&c->tripped, 1) != 0) return 0;
#if defined(_WIN32)
    (void)send((SOCKET)c->wr, "x", 1, 0);
#elif defined(__linux__)
    uint64_t one = 1;
    ssize_t w;
    do { w = write(c->wr, &one, sizeof one); } while (w < 0 && errno == EINTR);
#else
    ssize_t w;
    do { w = write(c->wr, "x", 1); } while (w < 0 && errno == EINTR);
#endif
    return 0;
}

int tcp_cancel_tripped_raw(TcpCancel* c) {
    return c && atomic_load(&c->tripped) ? 1 : 0;
}

/* Another reference to `c`, for a thread (or task, or actor) that may trip
 * it: it calls this before it is handed the handle and tcp_cancel_free_raw
 * once it can no longer trip it. Each holder's last use is then ordered
 * before the final drop that closes the descriptors and frees the handle
 * (the count is a sequentially consistent atomic), so a canceller's write
 * can never meet the owner's close. 0, or -1 for a null handle. */
int tcp_cancel_retain_raw(TcpCancel* c) {
    if (!c) return -1;
    net_cancel_ref(c);
    return 0;
}

/* Drop one reference: the creator's, or one taken by tcp_cancel_retain_raw.
 * A socket still bound to the handle keeps it until that socket is closed,
 * so freeing it while a connection is open is safe; tripping it after this
 * holder's call is not (its last use of it). The descriptors close and the
 * handle is freed when the last reference drops. */
void tcp_cancel_free_raw(TcpCancel* c) {
    if (c) net_cancel_unref(c);
}

#define NET_WAIT_READY      1
#define NET_WAIT_PERIOD     0    /* the operation's own timeout passed */
#define NET_WAIT_ERROR     (-1)
#define NET_WAIT_DEADLINE  (-3)
#define NET_WAIT_CANCELLED (-4)

/* Wait until `sock` is readable (or writable, `want_write`), no later than
 * its deadline and no longer than `period_ms` (-1 for no bound of its
 * own), and give up at once when its cancel is tripped: the cancel's
 * descriptor is polled beside the socket. NET_WAIT_READY, NET_WAIT_PERIOD,
 * NET_WAIT_DEADLINE or NET_WAIT_CANCELLED (each recorded in sock->stop),
 * or NET_WAIT_ERROR. A passed deadline and a tripped cancel are checked
 * before waiting at all, so both are sticky. The socket itself is only
 * polled, never closed or shut down here. */
static int net_wait_bound(TcpSocket* sock, int want_write, long long period_ms) {
    TcpCancel* c = sock->cancel;
    for (;;) {
        if (c && atomic_load(&c->tripped)) { sock->stop = TCP_STOP_CANCELLED; return NET_WAIT_CANCELLED; }
        long long wait = period_ms;
        int by_due = 0;
        if (sock->due_ms > 0) {
            long long left = sock->due_ms - net_mono_ms();
            if (left <= 0) { sock->stop = TCP_STOP_DEADLINE; return NET_WAIT_DEADLINE; }
            if (wait < 0 || left <= wait) { wait = left; by_due = 1; }
        }
#ifdef _WIN32
        fd_set rf, wf, ef;
        FD_ZERO(&rf); FD_ZERO(&wf); FD_ZERO(&ef);
        if (want_write) { FD_SET((SOCKET)sock->fd, &wf); FD_SET((SOCKET)sock->fd, &ef); }
        else FD_SET((SOCKET)sock->fd, &rf);
        if (c) FD_SET((SOCKET)c->rd, &rf);
        struct timeval tv, *tvp = NULL;
        if (wait >= 0) {
            tv.tv_sec = (long)(wait / 1000);
            tv.tv_usec = (long)((wait % 1000) * 1000);
            tvp = &tv;
        }
        int r = select(0, &rf, &wf, &ef, tvp);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) continue;
            return NET_WAIT_ERROR;
        }
        if (c && FD_ISSET((SOCKET)c->rd, &rf)) continue;   /* tripped: the top says so */
        if (r > 0) return NET_WAIT_READY;
#else
        struct pollfd pfd[2];
        pfd[0].fd = sock->fd;
        pfd[0].events = want_write ? POLLOUT : POLLIN;
        pfd[0].revents = 0;
        nfds_t n = 1;
        if (c) {
            pfd[1].fd = c->rd;
            pfd[1].events = POLLIN;
            pfd[1].revents = 0;
            n = 2;
        }
        int t = wait < 0 ? -1 : (wait > INT_MAX ? INT_MAX : (int)wait);
        int r = poll(pfd, n, t);
        if (r < 0) {
            if (errno == EINTR) continue;
            return NET_WAIT_ERROR;
        }
        if (n == 2 && pfd[1].revents) continue;   /* tripped: the top says so */
        if (r > 0 && pfd[0].revents) return NET_WAIT_READY;
#endif
        /* Nothing within `wait`: the deadline is checked again at the top
         * (a poll may wake a millisecond early), else the period passed. */
        if (!by_due) return NET_WAIT_PERIOD;
    }
}

/* The socket's own receive or send timeout (SO_RCVTIMEO / SO_SNDTIMEO) in
 * ms, -1 for none. */
static long long net_sock_timeout_ms(int fd, int opt) {
#ifdef _WIN32
    DWORD ms = 0;
    int len = (int)sizeof ms;
    if (getsockopt((SOCKET)fd, SOL_SOCKET, opt, (char*)&ms, &len) != 0 || ms == 0) return -1;
    return (long long)ms;
#else
    struct timeval tv = { 0, 0 };
    socklen_t tl = sizeof tv;
    if (getsockopt(fd, SOL_SOCKET, opt, &tv, &tl) != 0) return -1;
    long long ms = (long long)tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
    return ms > 0 ? ms : -1;
#endif
}

#ifdef _WIN32
#define NET_WOULDBLOCK() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR)
#define NET_RECV_NOWAIT 0
#else
#define NET_WOULDBLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#define NET_RECV_NOWAIT MSG_DONTWAIT
#endif

/* 1 when a deadline or a cancel is bound to `sock`. */
static int net_bound(const TcpSocket* sock) {
    return sock->due_ms > 0 || sock->cancel != NULL;
}

/* One receive on a socket with a deadline or a cancel bound to it: wait
 * (net_wait_bound) no later than the deadline, no longer than the socket's
 * own receive timeout, and not past a cancel, then take what is there.
 * The count (> 0), 0 for the peer's FIN, TCP_RECV_TIMEOUT (the receive
 * timeout passed: still connected, as an unbound receive's), TCP_RECV_
 * DEADLINE or TCP_RECV_CANCELLED (still connected: the owner closes), or
 * -1 for an error. */
static int net_recv_bound(TcpSocket* sock, void* buf, int len) {
    long long period = net_sock_timeout_ms(sock->fd, SO_RCVTIMEO);
    for (;;) {
        int w = net_wait_bound(sock, 0, period);
        if (w == NET_WAIT_DEADLINE) return TCP_RECV_DEADLINE;
        if (w == NET_WAIT_CANCELLED) return TCP_RECV_CANCELLED;
        if (w == NET_WAIT_PERIOD) return TCP_RECV_TIMEOUT;
        if (w != NET_WAIT_READY) return -1;
        int n = (int)recv(sock->fd, (char*)buf, len, NET_RECV_NOWAIT);
        if (n >= 0) return n;
        if (NET_WOULDBLOCK()) continue;   /* woken with nothing to read */
        return -1;
    }
}

struct TcpServer {
    int fd;
    int port;
    /* The socket file a Unix-domain listener bound, removed when it is
     * closed; NULL for a TCP listener. */
    char* unix_path;
};

/* Read and set by whichever thread opens the first socket, so atomic, as
 * the HTTP server's is (#3680). Two threads that both find it clear both
 * start Winsock, which counts its starts. */
static _Atomic int net_initialized = 0;

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
#if defined(AETHER_NO_THREADING)
/* A cooperative build's mutex is a no-op, and with task contexts several
 * tasks dial at once: the memo stays shared (what one task learned about a
 * host serves the next dial to it), under the context lock (aether_frames.h). */
#include "../../runtime/actors/aether_frames.h"
static atomic_flag net_family_memo_flag = ATOMIC_FLAG_INIT;
#define net_family_memo_enter() aether_context_lock(&net_family_memo_flag)
#define net_family_memo_leave() aether_context_unlock(&net_family_memo_flag)
#elif defined(_WIN32)
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
#if !defined(AETHER_NO_THREADING)
#define net_family_memo_enter() pthread_mutex_lock(net_family_memo_lock())
#define net_family_memo_leave() pthread_mutex_unlock(net_family_memo_lock())
#endif

static int net_family_lookup(const char* host, int port) {
    int family = 0;
    if (!host || strlen(host) >= sizeof net_family_memo[0].host) return 0;
    net_family_memo_enter();
    for (int i = 0; i < NET_FAMILY_MEMO_SLOTS; i++) {
        if (net_family_memo[i].family && net_family_memo[i].port == port &&
            strcmp(net_family_memo[i].host, host) == 0) {
            family = net_family_memo[i].family;
            break;
        }
    }
    net_family_memo_leave();
    return family;
}

void aether_net_note_family(const char* host, int port, int family) {
    if (!host || strlen(host) >= sizeof net_family_memo[0].host) return;
    net_family_memo_enter();
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
        memcpy(net_family_memo[slot].host, host, strlen(host) + 1);   /* fits: checked above */
        net_family_memo[slot].port = port;
    }
    net_family_memo[slot].family = family;
    net_family_memo_leave();
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

/* aether_net_connect_addr, and with `cancel` the wait for the connection
 * also ends when it is tripped: "cancelled", *timed_out 2. With a cancel the
 * connect is non-blocking even with no timeout, so that it can be woken. */
static int net_connect_addr_ex(const struct sockaddr_storage* addr, socklen_t len,
                               long long timeout_ns, int read_buffer, TcpCancel* cancel,
                               const char** why, int* timed_out) {
    *timed_out = 0;
    int sockfd = (int)socket(addr->ss_family, SOCK_STREAM, 0);
    if (sockfd < 0) {
        *why = "could not create socket";
        return -1;
    }
    /* Before connect, so the SYN advertises a window this size (#3338). Set
     * on a connected socket, the buffer shrinks, but the kernel still takes
     * the window it already advertised: on Linux a reader that stopped for
     * three seconds then drained what it had taken for sixteen more. */
    if (read_buffer > 0 &&
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const char*)&read_buffer,
                   sizeof(read_buffer)) != 0) {
        close(sockfd);
        *why = "could not set the receive buffer";
        return -1;
    }
    if (timeout_ns <= 0 && !cancel) {
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
        fd_set wfds, efds, rfds;
        FD_ZERO(&wfds); FD_SET(sockfd, &wfds);
        FD_ZERO(&efds); FD_SET(sockfd, &efds);
        FD_ZERO(&rfds);
        if (cancel) FD_SET((SOCKET)cancel->rd, &rfds);
        struct timeval tv, *tvp = NULL;
        if (timeout_ns > 0) {
            tv.tv_sec  = (long)(timeout_ns / 1000000000LL);
            tv.tv_usec = (long)((timeout_ns / 1000LL) % 1000000LL);
            tvp = &tv;
        }
        int sel = select(sockfd + 1, &rfds, &wfds, &efds, tvp);
        int woken = cancel && sel > 0 && FD_ISSET((SOCKET)cancel->rd, &rfds);
#else
        /* poll, not select: a POSIX fd_set is a bitmap of FD_SETSIZE bits,
         * and FD_SET on a descriptor at or above it writes past the end. A
         * process serving many connections (a proxy, which is what dials
         * most often) passes 1024 descriptors easily. POLLOUT covers both
         * outcomes: a refused non-blocking connect reports the socket ready
         * with SO_ERROR set, which the check below reads. */
        struct pollfd pfd[2];
        pfd[0].fd = sockfd;
        pfd[0].events = POLLOUT;
        pfd[0].revents = 0;
        nfds_t npfd = 1;
        if (cancel) {
            pfd[1].fd = cancel->rd;
            pfd[1].events = POLLIN;
            pfd[1].revents = 0;
            npfd = 2;
        }
        long long timeout_ms = timeout_ns > 0 ? (timeout_ns + 999999LL) / 1000000LL : -1;
        if (timeout_ms > INT_MAX) timeout_ms = INT_MAX;
        int sel;
        do { sel = poll(pfd, npfd, (int)timeout_ms); } while (sel < 0 && errno == EINTR);
        int woken = npfd == 2 && sel > 0 && pfd[1].revents != 0;
#endif
        if (woken || (cancel && atomic_load(&cancel->tripped))) {
            close(sockfd);
            *why = TCP_ERR_CANCELLED;
            *timed_out = 2;
            return -1;
        }
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

int aether_net_connect_addr(const struct sockaddr_storage* addr, socklen_t len,
                            long long timeout_ns, int read_buffer,
                            const char** why, int* timed_out) {
    return net_connect_addr_ex(addr, len, timeout_ns, read_buffer, NULL, why, timed_out);
}

/* aether_net_dial_addrs, ended early by `cancel` (NULL for none). */
static int net_dial_addrs_ex(const char* host, int port, AetherAddrList* addrs,
                             long long timeout_ns, int read_buffer, TcpCancel* cancel,
                             const char** why);

int aether_net_dial_addrs(const char* host, int port, AetherAddrList* addrs,
                          long long timeout_ns, int read_buffer, const char** why) {
    return net_dial_addrs_ex(host, port, addrs, timeout_ns, read_buffer, NULL, why);
}

static int net_dial_addrs_ex(const char* host, int port, AetherAddrList* addrs,
                             long long timeout_ns, int read_buffer, TcpCancel* cancel,
                             const char** why) {
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
            int fd = net_connect_addr_ex(&addrs->ss[i], addrs->len[i], t, read_buffer, cancel,
                                         why, &timed_out);
            if (timed_out == 2) return -1;   /* cancelled */
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

/* A write to a connection the peer has closed raises SIGPIPE, which ends a
 * process that has not ignored it, with no word said (#3457): the pure TLS
 * stack's peer died writing to a client that had stopped reading, and its
 * client writing after the peer closed, on macOS, whose socket buffers are
 * small enough for a write to come after the close. The write fails with
 * EPIPE instead, as the HTTP server's do: Linux names it on each send
 * (MSG_NOSIGNAL), macOS and the BSDs on the socket (SO_NOSIGPIPE), and
 * Windows raises no signal. */
#if defined(MSG_NOSIGNAL)
#define AE_NET_NOSIGNAL MSG_NOSIGNAL
#else
#define AE_NET_NOSIGNAL 0
#endif
static void net_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

/* tcp_connect_raw, and with `read_buffer` > 0 the receive buffer set before
 * the connection is made (tcp_connect_buffered_raw), and with `timeout_ns` >
 * 0 the whole dial, every address the host resolves to, held to that budget
 * (tcp_connect_timeout_raw). NULL on failure, with why in `*why`. */
static TcpSocket* tcp_connect_with(const char* host, int port, int read_buffer,
                                   long long timeout_ns, const char** why) {
    // Sandbox check: is TCP connect to this host and port allowed?
    char tcp_res[300];
    if (!aether_sandbox_check("tcp", aether_net_resource(tcp_res, sizeof tcp_res, host, port))) {
        *why = "denied by sandbox";
        return NULL;
    }

    aether_net_init();

    /* getaddrinfo, not gethostbyname: the latter returns a pointer into a
     * shared, process-static struct, so concurrent connects on different
     * threads race and corrupt each other's resolved address. getaddrinfo is
     * thread-safe and returns caller-owned memory. Each address the host has,
     * IPv6 or IPv4, is tried as aether_net_dial_addrs describes. */
    AetherAddrList addrs;
    if (aether_addr_resolve(host, port, &addrs) != 0) {
        *why = "could not resolve host";
        return NULL;
    }
    int sockfd = aether_net_dial_addrs(host, port, &addrs, timeout_ns, read_buffer, why);
    if (sockfd < 0) return NULL;

    net_set_socket_timeouts(sockfd, 30);
    net_no_sigpipe(sockfd);

    TcpSocket* sock = net_sock_new(sockfd);
    if (!sock) { *why = "out of memory"; return NULL; }
    return sock;
}

TcpSocket* tcp_connect_raw(const char* host, int port) {
    const char* why = NULL;
    return tcp_connect_with(host, port, 0, 0, &why);
}

TcpSocket* tcp_connect_buffered_raw(const char* host, int port, int read_buffer) {
    if (read_buffer <= 0) return NULL;
    const char* why = NULL;
    return tcp_connect_with(host, port, read_buffer, 0, &why);
}

/* A connect that gives up after `timeout_ms`: each address is dialled
 * non-blocking and polled (aether_net_connect_addr), so an address that
 * drops the SYN costs the budget, not the kernel's own retry schedule
 * (75 s on macOS, about two minutes on Linux). "connect timeout" when the
 * budget runs out. */
TcpOpenResult tcp_connect_timeout_raw(const char* host, int port, int timeout_ms) {
    TcpOpenResult out = { NULL, "connection failed" };
    if (timeout_ms <= 0) { out._1 = "timeout_ms must be positive"; return out; }
    const char* why = "connection failed";
    TcpSocket* sock = tcp_connect_with(host, port, 0, (long long)timeout_ms * 1000000LL, &why);
    if (!sock) { out._1 = why ? why : "connection failed"; return out; }
    out._0 = sock;
    out._1 = "";
    return out;
}

/* Make `fd` non-blocking for a bound send; returns what to hand to
 * net_nonblock_end. On Windows a socket's mode cannot be read back; std.tcp's
 * are blocking, so it goes back to blocking (as tcp_send_some_raw does). */
static int net_nonblock_begin(int fd) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &on) == 0 ? 1 : 0;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || (fl & O_NONBLOCK)) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 ? fl : -1;
#endif
}

static void net_nonblock_end(int fd, int saved) {
#ifdef _WIN32
    if (saved) {
        u_long off = 0;
        ioctlsocket((SOCKET)fd, FIONBIO, &off);
    }
#else
    if (saved >= 0) {
        int e = errno;
        (void)fcntl(fd, F_SETFL, saved);
        errno = e;
    }
#endif
}

/* Send on a socket with a deadline or a cancel bound to it: `all` 1 sends
 * every byte (tcp_send_all_raw), 0 returns after the first send that takes
 * any (tcp_send_raw and its kin). The socket is non-blocking for the
 * sends, so none waits past what net_wait_bound allows, and the socket's
 * own send timeout keeps its meaning: a period of it in which nothing went
 * ends the send, one in which something did starts another (#3108). A
 * passed deadline or a tripped cancel ends it with sock->stop saying which,
 * checked before the first byte too. Returns the bytes sent, or -1 when
 * nothing was sent and the send failed or stopped. Do not send on the
 * socket from another thread at the same time. */
static int net_send_bound(TcpSocket* sock, const char* p, int length, int all) {
    if (sock->cancel && atomic_load(&sock->cancel->tripped)) { sock->stop = TCP_STOP_CANCELLED; return -1; }
    if (sock->due_ms > 0 && net_mono_ms() >= sock->due_ms) { sock->stop = TCP_STOP_DEADLINE; return -1; }
    long long period = net_sock_timeout_ms(sock->fd, SO_SNDTIMEO);
    long long period_due = 0;
    int progressed = 0;
    int sent = 0;
    int saved = net_nonblock_begin(sock->fd);
    while (sent < length) {
        int n = (int)send(sock->fd, p + sent, length - sent, AE_NET_NOSIGNAL);
        if (n > 0) {
            sent += n;
            progressed = 1;
            if (!all) break;
            continue;
        }
        if (n < 0 && NET_WOULDBLOCK()) {
            long long wait = -1;
            if (period > 0) {
                long long now = net_mono_ms();
                if (!period_due) period_due = now + period;
                if (period_due - now <= 0) {
                    if (!progressed) break;      /* a whole period, nothing went */
                    progressed = 0;
                    period_due = now + period;
                }
                wait = period_due - now;
            }
            int w = net_wait_bound(sock, 1, wait);
            if (w == NET_WAIT_READY || w == NET_WAIT_PERIOD) continue;
            break;                                /* deadline, cancel or error */
        }
        break;                                    /* the connection failed */
    }
    net_nonblock_end(sock->fd, saved);
    return sent > 0 ? sent : -1;
}

/* Bind `due_ms` (monotonic ms, 0 for none) and `cancel` (NULL for none) to
 * `sock`, replacing what was bound, and clear its stop: every receive and
 * send on it from now on is bounded by them (tcp_poll_raw and its kin,
 * which take a timeout of their own, and tcp_send_some_raw, which never
 * waits, are not). The socket takes a reference to `cancel`, dropped when it
 * is closed or something else is bound. 0, or -1 for a null handle or a
 * negative deadline. */
int tcp_set_deadline_raw(TcpSocket* sock, long long due_ms, TcpCancel* cancel) {
    if (!sock || due_ms < 0) return -1;
    if (cancel) net_cancel_ref(cancel);
    if (sock->cancel) net_cancel_unref(sock->cancel);
    sock->cancel = cancel;
    sock->due_ms = due_ms;
    sock->stop = TCP_STOP_NONE;
    return 0;
}

long long tcp_deadline_raw(TcpSocket* sock) {
    return sock ? sock->due_ms : 0;
}

/* What stopped the last operation a bound deadline or cancel stopped on
 * `sock`: TCP_STOP_DEADLINE, TCP_STOP_CANCELLED, or TCP_STOP_NONE. */
int tcp_stop_raw(TcpSocket* sock) {
    return sock ? sock->stop : TCP_STOP_NONE;
}

/* tcp_connect_raw held to the absolute deadline `due_ms` (aether_now_ns's
 * monotonic clock, in ms; 0 for none) and ended by `cancel` (NULL for none)
 * while it waits for the connection, every address the host resolves to;
 * the socket comes back with both bound (tcp_set_deadline_raw), so the
 * same deadline goes on covering every receive and send. Resolving the
 * name is not interrupted (getaddrinfo cannot be): the deadline and the
 * cancel are checked before and after it, so a numeric address is the
 * fully bounded dial. "deadline exceeded" and "cancelled" are the two
 * stops; otherwise the errors are tcp_connect_timeout_raw's. */
TcpOpenResult tcp_connect_until_raw(const char* host, int port, long long due_ms, TcpCancel* cancel) {
    TcpOpenResult out = { NULL, "connection failed" };
    if (due_ms < 0) { out._1 = "due_ms must be 0 (no deadline) or a time on the monotonic clock"; return out; }
    if (cancel && atomic_load(&cancel->tripped)) { out._1 = TCP_ERR_CANCELLED; return out; }
    if (due_ms > 0 && net_mono_ms() >= due_ms) { out._1 = TCP_ERR_DEADLINE; return out; }
    char tcp_res[300];
    if (!aether_sandbox_check("tcp", aether_net_resource(tcp_res, sizeof tcp_res, host, port))) {
        out._1 = "denied by sandbox";
        return out;
    }
    aether_net_init();
    AetherAddrList addrs;
    if (aether_addr_resolve(host, port, &addrs) != 0) { out._1 = "could not resolve host"; return out; }
    if (cancel && atomic_load(&cancel->tripped)) { out._1 = TCP_ERR_CANCELLED; return out; }
    long long timeout_ns = 0;
    if (due_ms > 0) {
        long long left = due_ms - net_mono_ms();
        if (left <= 0) { out._1 = TCP_ERR_DEADLINE; return out; }
        timeout_ns = left * 1000000LL;
    }
    const char* why = "connection failed";
    int fd = net_dial_addrs_ex(host, port, &addrs, timeout_ns, 0, cancel, &why);
    if (fd < 0) {
        if (cancel && atomic_load(&cancel->tripped)) why = TCP_ERR_CANCELLED;
        else if (due_ms > 0 && net_mono_ms() >= due_ms) why = TCP_ERR_DEADLINE;
        out._1 = why ? why : "connection failed";
        return out;
    }
    net_set_socket_timeouts(fd, 30);
    net_no_sigpipe(fd);
    TcpSocket* sock = net_sock_new(fd);
    if (!sock) { out._1 = "out of memory"; return out; }
    tcp_set_deadline_raw(sock, due_ms, cancel);
    out._0 = sock;
    out._1 = "";
    return out;
}

int tcp_send_raw(TcpSocket* sock, const char* data) {
    if (!sock || !sock->connected || !data) return -1;
    if (net_bound(sock)) return net_send_bound(sock, data, (int)strlen(data), 0);

    int sent = send(sock->fd, data, strlen(data), AE_NET_NOSIGNAL);
    return sent;
}

int tcp_send_n_raw(TcpSocket* sock, const char* data, int length) {
    if (!sock || !sock->connected || !data || length < 0) return -1;
    if (length == 0) return 0;
    if (net_bound(sock)) return net_send_bound(sock, data, length, 0);

    int sent = send(sock->fd, data, (size_t)length, AE_NET_NOSIGNAL);
    return sent;
}

char* tcp_receive_raw(TcpSocket* sock, int max_bytes) {
    if (!sock || !sock->connected || max_bytes <= 0) return NULL;

    /* Cap-aware (#343): max_bytes is caller-supplied (program-
     * controlled, but plugin-host paths surface it as untrusted).
     * The buffer becomes the payload of the counted string returned. */
    char* buffer = (char*)aether_caps_malloc((size_t)max_bytes + 1);
    if (!buffer) return NULL;
    int received = 0;
    if (net_bound(sock)) {
        received = net_recv_bound(sock, buffer, max_bytes);
        if (received < -1) {
            /* A receive timeout, a passed deadline or a cancel: the socket
             * stays connected for its owner to close. */
            aether_caps_free(buffer, (size_t)max_bytes + 1);
            return NULL;
        }
    } else {
        received = net_recv(sock->fd, buffer, max_bytes);
    }

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

    int received = 0;
    if (net_bound(sock)) {
        received = net_recv_bound(sock, buffer, max_bytes);
        if (received < -1) {
            aether_caps_free(buffer, (size_t)max_bytes);
            /* Distinct from each other and from a close: the socket stays
             * connected, and its owner closes it. */
            if (received == TCP_RECV_DEADLINE) out._2 = TCP_ERR_DEADLINE;
            else if (received == TCP_RECV_CANCELLED) out._2 = TCP_ERR_CANCELLED;
            else out._2 = "timeout";
            return out;
        }
    } else {
        received = net_recv(sock->fd, buffer, max_bytes);
    }
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
    if (net_bound(sock)) return net_send_bound(sock, (const char*)data, length, 0);
    int sent = send(sock->fd, (const char*)data, (size_t)length, AE_NET_NOSIGNAL);
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
    if (net_bound(sock)) {
        int n = net_send_bound(sock, (const char*)data, length, 1);
        return n < 0 ? 0 : n;
    }
    const char* p = (const char*)data;
    int sent = 0;
#ifndef _WIN32
    NetSendWait wait = { -1, 0, 0 };
#endif
#if defined(__NetBSD__)
    /* NetBSD's send(2) does not take MSG_DONTWAIT on a TCP socket: a send
     * to a full buffer blocked until SO_SNDTIMEO, or for good with none, so
     * the wait below never ran. The socket is non-blocking for the loop. */
    int nb_flags = fcntl(sock->fd, F_GETFL, 0);
    int nb_set = 0;
    if (nb_flags >= 0 && !(nb_flags & O_NONBLOCK)) {
        /* The wait's bound is read now, while the socket is still blocking:
         * net_wait_writable takes a non-blocking one as a caller's
         * "do not wait". */
        struct timeval tv = { 0, 0 };
        socklen_t tl = sizeof tv;
        wait.timeout_ms = getsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, &tl) == 0
                        ? (long long)tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000 : 0;
        nb_set = fcntl(sock->fd, F_SETFL, nb_flags | O_NONBLOCK) == 0;
    }
#endif
    while (sent < length) {
#ifdef _WIN32
        int n = send(sock->fd, p + sent, (size_t)(length - sent), AE_NET_NOSIGNAL);
        if (n > 0) {
            sent += n;
            continue;
        }
        if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
#else
        int n = (int)send(sock->fd, p + sent, (size_t)(length - sent), MSG_DONTWAIT | AE_NET_NOSIGNAL);
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
#if defined(__NetBSD__)
    if (nb_set) {
        int saved = errno;
        (void)fcntl(sock->fd, F_SETFL, nb_flags);
        errno = saved;
    }
#endif
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
    int received = 0;
    if (net_bound(sock)) {
        received = net_recv_bound(sock, buf, cap);
        if (received < -1) return received;   /* TIMEOUT, DEADLINE, CANCELLED */
    } else {
        received = net_recv(sock->fd, buf, cap);
    }
    if (received > 0) return received;
    if (received < 0 && aether_net_wouldblock()) return TCP_RECV_TIMEOUT;
    sock->connected = 0;
    return TCP_RECV_CLOSED;
}

int tcp_close(TcpSocket* sock) {
    if (!sock) return -1;

    /* A peer EOF/error clears connected, but the handle still owns fd. The
     * one close of the descriptor is here, by the handle's owner: neither a
     * passed deadline nor a cancel closes it (see TcpCancel). */
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
        sock->connected = 0;
    }
    if (sock->cancel) {
        net_cancel_unref(sock->cancel);
        sock->cancel = NULL;
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
    server->unix_path = NULL;
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
 * (Linux doubles it for its bookkeeping). Setting it asks the kernel not to
 * grow the buffer by itself, as Linux, macOS and Windows otherwise do while
 * data arrives, read or not. On a connected socket the window it advertised
 * at connect still stands (tcp_connect_buffered_raw sets the size first,
 * #3338). The size is a request a kernel may round up. */
int tcp_set_read_buffer_raw(TcpSocket* sock, int bytes) {
    if (!sock || !sock->connected || bytes <= 0) return -1;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    return 0;
}

/* How many received bytes are waiting to be read on `sock` (FIONREAD), or
 * -1 on error. */
int tcp_available_raw(TcpSocket* sock) {
    if (!sock || !sock->connected) return -1;
#ifdef _WIN32
    u_long n = 0;
    if (ioctlsocket(sock->fd, FIONREAD, &n) != 0) return -1;
    return n > (u_long)INT_MAX ? INT_MAX : (int)n;
#else
    int n = 0;
    if (ioctl(sock->fd, FIONREAD, &n) != 0) return -1;
    return n;
#endif
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
    net_no_sigpipe(newsockfd);

    return net_sock_new(newsockfd);
}

int tcp_server_close(TcpServer* server) {
    if (!server) return -1;

    close(server->fd);
    /* A Unix-domain listener's socket file outlives the socket: remove it,
     * so the next listen on the path does not find it in use. */
    if (server->unix_path) {
        remove(server->unix_path);
        free(server->unix_path);
    }
    free(server);
    return 0;
}

/* ---- Unix-domain stream sockets --------------------------------------- */

/* `path` as a sockaddr_un. A path that does not fit sun_path with its NUL
 * is refused: truncating it would connect to (or bind) a different file. */
static int net_unix_addr(const char* path, struct sockaddr_un* sa, socklen_t* len,
                         const char** why) {
    if (!path || !path[0]) { *why = "empty socket path"; return -1; }
    size_t n = strlen(path);
    memset(sa, 0, sizeof *sa);
    if (n >= sizeof sa->sun_path) {
        *why = "socket path too long: sockaddr_un holds 103 bytes on macOS and the BSDs, "
               "107 on Linux and Windows";
        return -1;
    }
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path, path, n + 1);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
    sa->sun_len = (unsigned char)*len;
#endif
    return 0;
}

/* A socket() for AF_UNIX that failed: on Windows before 10 1803 the family
 * itself is missing. */
static const char* net_unix_socket_why(void) {
#ifdef _WIN32
    int err = WSAGetLastError();
    if (err == WSAEAFNOSUPPORT || err == WSAEPROTONOSUPPORT)
        return "Unix-domain sockets are not supported on this Windows (they need Windows 10 1803 or later)";
#else
    if (errno == EAFNOSUPPORT) return "Unix-domain sockets are not supported on this platform";
#endif
    return "could not create socket";
}

int aether_net_dial_unix(const char* path, const char** why) {
    *why = "connection failed";
    if (!path || !path[0]) { *why = "empty socket path"; return -1; }
    /* The kernel's rule, in process: connecting to a Unix socket needs write
     * permission on its file. A socket in a directory the program may write
     * is one it may talk to, and one it may not write is refused. */
    if (!aether_sandbox_check("fs_write", path)) {
        *why = "denied by sandbox: a Unix socket connect needs fs_write on its path";
        return -1;
    }
    struct sockaddr_un sa;
    socklen_t len = 0;
    if (net_unix_addr(path, &sa, &len, why) != 0) return -1;

    aether_net_init();
    int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *why = net_unix_socket_why(); return -1; }
    /* A write after the peer closed fails with EPIPE at once on a Unix
     * socket (a TCP one takes a reset first), and a pooled HTTP connection
     * the daemon closed is written before it is found closed: that must be
     * an error the client retries on, not a SIGPIPE that ends the process.
     * Linux says so per send (MSG_NOSIGNAL), macOS and the BSDs here. */
    net_no_sigpipe(fd);
    for (;;) {
        if (connect(fd, (const struct sockaddr*)&sa, len) == 0) return fd;
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAECONNREFUSED) *why = "connection refused: nothing is listening on the socket";
        else if (err == WSAEACCES) *why = "permission denied";
        else *why = "connection failed";
#else
        if (errno == EINTR) continue;
        if (errno == ENOENT) *why = "no such socket";
        else if (errno == ECONNREFUSED) *why = "connection refused: nothing is listening on the socket";
        else if (errno == EACCES || errno == EPERM) *why = "permission denied";
        else if (errno == ENOTSOCK || errno == EPROTOTYPE) *why = "not a stream socket";
        else if (errno == ENOTDIR) *why = "no such socket";
        else *why = "connection failed";
#endif
        break;
    }
    close(fd);
    return -1;
}

TcpOpenResult tcp_connect_unix_raw(const char* path) {
    TcpOpenResult out = { NULL, "connection failed" };
    const char* why = NULL;
    int fd = aether_net_dial_unix(path, &why);
    if (fd < 0) { out._1 = why; return out; }
    /* The same as a TCP connect's from here: 30 s send/receive timeouts (a
     * quiet peer reads as "timeout", not a close) and no SIGPIPE. */
    net_set_socket_timeouts(fd, 30);
    net_no_sigpipe(fd);
    TcpSocket* sock = net_sock_new(fd);
    if (!sock) { out._1 = "out of memory"; return out; }
    out._0 = sock;
    out._1 = "";
    return out;
}

TcpOpenResult tcp_listen_unix_raw(const char* path) {
    TcpOpenResult out = { NULL, "listen failed" };
    if (!aether_sandbox_check("tcp_listen", "*") || !aether_sandbox_check("fs_write", path ? path : "")) {
        out._1 = "denied by sandbox: a Unix socket listener needs tcp_listen and fs_write on its path";
        return out;
    }
    struct sockaddr_un sa;
    socklen_t len = 0;
    const char* why = NULL;
    if (net_unix_addr(path, &sa, &len, &why) != 0) { out._1 = why; return out; }

    aether_net_init();
    int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { out._1 = net_unix_socket_why(); return out; }
    if (bind(fd, (const struct sockaddr*)&sa, len) != 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        out._1 = err == WSAEADDRINUSE ? "address in use: a file already exists at the socket path"
               : err == WSAEACCES     ? "permission denied" : "bind failed";
#else
        out._1 = errno == EADDRINUSE ? "address in use: a file already exists at the socket path"
               : (errno == EACCES || errno == EPERM) ? "permission denied"
               : (errno == ENOENT || errno == ENOTDIR) ? "no such directory for the socket path"
               : "bind failed";
#endif
        close(fd);
        return out;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        remove(path);
        return out;
    }
    TcpServer* server = (TcpServer*)malloc(sizeof(TcpServer));
    char* kept = strdup(path);
    if (!server || !kept) {
        free(server);
        free(kept);
        close(fd);
        remove(path);
        out._1 = "out of memory";
        return out;
    }
    server->fd = fd;
    server->port = -1;
    server->unix_path = kept;
    out._0 = server;
    out._1 = "";
    return out;
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

/* A connected descriptor as a std.tcp socket, blocking; `default_timeouts`
 * puts std.tcp's 30 s receive and send timeouts on it, else it keeps the
 * ones its owner set. */
static TcpSocket* tcp_socket_wrap_fd(int fd, int default_timeouts) {
    if (fd < 0) return NULL;
    aether_net_init();
#ifndef _WIN32
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
    if (default_timeouts) net_set_socket_timeouts(fd, 30);
    net_no_sigpipe(fd);

    return net_sock_new(fd);
}

TcpSocket* tcp_socket_from_fd_owned(int fd) {
    return tcp_socket_wrap_fd(fd, 1);
}

/* As tcp_socket_from_fd_owned, keeping the receive and send timeouts the
 * descriptor's owner put on it (#3703): the pure-Aether TLS server's accept
 * takes the HTTP server's descriptor this way, so its handshake is bounded
 * by the server's receive timeout, not std.tcp's 30 s. */
TcpSocket* tcp_socket_adopt_fd(int fd) {
    return tcp_socket_wrap_fd(fd, 0);
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

/* Wait until a socket has room to send: 1 when a send will take bytes (or
 * the socket has an error or hangup, which the send then reports), 0 on
 * timeout, -1 on a null/closed handle or a poll error. Sends nothing. */
int tcp_wait_writable_raw(TcpSocket* sock, int timeout_ms) {
    if (!sock || !sock->connected) return -1;
    AE_POLLFD pfd;
    pfd.fd = sock->fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int rc = ae_poll(&pfd, (ae_nfds_t)1, timeout_ms);
    if (rc < 0) {
        if (aether_net_wouldblock()) return 0;   /* EINTR: treat as no-event */
        return -1;
    }
    if (rc == 0) return 0;
    if (pfd.revents & POLLNVAL) return -1;
    return 1;
}

/* What the socket takes now of `length` bytes, never waiting for room (the
 * write side of a UI thread's loop, which must not wait on a peer that has
 * stopped reading): the count, 0 when the send buffer is full, -1 on an
 * error (which marks the socket dead, as a failed receive does).
 *
 * POSIX sends with MSG_DONTWAIT, except where it does not keep a send from
 * waiting: NetBSD ignores it on a TCP socket (tcp_send_all_raw's note), and
 * macOS sends what fits and then waits for room for the rest, up to the
 * send timeout (a 64 KiB send to a full loopback connection took 35 s).
 * There, and on Windows, which has no such flag, the socket is made
 * non-blocking for the one send and put back.
 * Windows cannot read that mode back; std.tcp's sockets are blocking, so it
 * is put back blocking. Do not send on the socket from another thread at
 * the same time. */
int tcp_send_some_raw(TcpSocket* sock, const void* data, int length) {
    if (!sock || !sock->connected || length < 0) return -1;
    if (length == 0) return 0;
    if (!data) return -1;
    int n;
#if defined(_WIN32)
    u_long on = 1, off = 0;
    if (ioctlsocket(sock->fd, FIONBIO, &on) != 0) return -1;
    n = send(sock->fd, (const char*)data, length, 0);
    int werr = n < 0 ? WSAGetLastError() : 0;
    ioctlsocket(sock->fd, FIONBIO, &off);
    if (n < 0) {
        if (werr == WSAEWOULDBLOCK || werr == WSAEINTR) return 0;
        sock->connected = 0;
        return -1;
    }
    return n;
#else
#if defined(__NetBSD__) || defined(__APPLE__)
    int fl = fcntl(sock->fd, F_GETFL, 0);
    int set = fl >= 0 && !(fl & O_NONBLOCK) && fcntl(sock->fd, F_SETFL, fl | O_NONBLOCK) == 0;
    do { n = (int)send(sock->fd, data, (size_t)length, AE_NET_NOSIGNAL); } while (n < 0 && errno == EINTR);
    int e = errno;
    if (set) (void)fcntl(sock->fd, F_SETFL, fl);
    errno = e;
#else
    do { n = (int)send(sock->fd, data, (size_t)length, MSG_DONTWAIT | AE_NET_NOSIGNAL); } while (n < 0 && errno == EINTR);
#endif
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        sock->connected = 0;
        return -1;
    }
    return n;
#endif
}

#endif // AETHER_HAS_NETWORKING
