#include "aether_net.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"
#include "../string/aether_string.h"

#if !AETHER_HAS_NETWORKING
TcpSocket* tcp_connect_raw(const char* h, int p) { (void)h; (void)p; return NULL; }
int tcp_send_raw(TcpSocket* s, const char* d) { (void)s; (void)d; return -1; }
int tcp_send_n_raw(TcpSocket* s, const char* d, int n) { (void)s; (void)d; (void)n; return -1; }
char* tcp_receive_raw(TcpSocket* s, int m) { (void)s; (void)m; return NULL; }
TcpReceiveResult tcp_receive_n_raw(TcpSocket* s, int m) {
    (void)s; (void)m;
    TcpReceiveResult out = { NULL, 0, "net unavailable" };
    return out;
}
int tcp_close(TcpSocket* s) { (void)s; return 0; }
TcpServer* tcp_listen_raw(int p) { (void)p; return NULL; }
TcpSocket* tcp_accept_raw(TcpServer* s) { (void)s; return NULL; }
int tcp_server_close(TcpServer* s) { (void)s; return 0; }
int tcp_fd_raw(TcpSocket* s) { (void)s; return -1; }
int tcp_server_fd_raw(TcpServer* s) { (void)s; return -1; }
TcpSocket* tcp_socket_from_fd_owned(int fd) { (void)fd; return NULL; }
int tcp_poll_raw(TcpSocket* s, int t) { (void)s; (void)t; return -1; }
int tcp_poll2_raw(TcpSocket* a, TcpSocket* b, int t) { (void)a; (void)b; (void)t; return -1; }
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
    #include <poll.h>
    #include <fcntl.h>
#endif

/* recv returned <= 0: distinguish "peer idle, socket still alive"
 * (SO_RCVTIMEO fired, or a nonblocking socket with nothing buffered) from
 * "peer closed / hard error". A 30 s quiet window on a long-lived tunnel is
 * normal and must NOT tear the connection down — see issue #1092. Returns 1
 * for would-block/timeout/interrupt, 0 for a genuine close-or-error. Only
 * meaningful when `received < 0`; an orderly FIN is received == 0. */
static int net_recv_wouldblock(void) {
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

static void net_init() {
    if (net_initialized) return;
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
    net_initialized = 1;
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

TcpSocket* tcp_connect_raw(const char* host, int port) {
    // Sandbox check: is TCP connect to this host allowed?
    if (!aether_sandbox_check("tcp", host)) return NULL;

    net_init();

    /* getaddrinfo, not gethostbyname: the latter returns a pointer into a
     * shared, process-static struct, so concurrent connects on different
     * threads race and corrupt each other's resolved address. getaddrinfo is
     * thread-safe and returns caller-owned memory. Pinned to AF_INET to match
     * the sockaddr_in the rest of this function builds. */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = NULL;
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
            return NULL;
        }
        memcpy(&serv_addr, res->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(res);
    }
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return NULL;
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return NULL;
    }

    net_set_socket_timeouts(sockfd, 30);

    TcpSocket* sock = (TcpSocket*)malloc(sizeof(TcpSocket));
    if (!sock) { close(sockfd); return NULL; }
    sock->fd = sockfd;
    sock->connected = 1;
    return sock;
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
     * Caller frees with plain libc free per the caller-owned-return
     * contract (aether_resource_caps.h:89-94). */
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
        if (!(received < 0 && net_recv_wouldblock())) {
            sock->connected = 0;
        }
        return NULL;
    }

    buffer[received] = '\0';
    return buffer;
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
        if (received < 0 && net_recv_wouldblock()) {
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

int tcp_close(TcpSocket* sock) {
    if (!sock) return -1;

    if (sock->connected) {
        close(sock->fd);
        sock->connected = 0;
    }

    free(sock);
    return 0;
}

TcpServer* tcp_listen_raw(int port) {
    // Sandbox check: is listening on this port allowed?
    if (!aether_sandbox_check("tcp_listen", "*")) return NULL;

    net_init();

    // Validate port range (1-65535)
    if (port < 1 || port > 65535) {
        return NULL;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return NULL;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return NULL;
    }

    if (listen(sockfd, 5) < 0) {
        close(sockfd);
        return NULL;
    }

    TcpServer* server = (TcpServer*)malloc(sizeof(TcpServer));
    if (!server) { close(sockfd); return NULL; }
    server->fd = sockfd;
    server->port = port;
    return server;
}

TcpSocket* tcp_accept_raw(TcpServer* server) {
    if (!server) return NULL;

    struct sockaddr_in cli_addr;
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
    net_init();
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
int tcp_poll_raw(TcpSocket* sock, int timeout_ms) {
    if (!sock || !sock->connected) return -1;
    AE_POLLFD pfd;
    pfd.fd = sock->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = ae_poll(&pfd, (ae_nfds_t)1, timeout_ms);
    if (rc < 0) {
        if (net_recv_wouldblock()) return 0;   /* EINTR: treat as no-event */
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
        if (net_recv_wouldblock()) return 0;   /* EINTR: caller re-polls */
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
