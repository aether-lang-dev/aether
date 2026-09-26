#include "aether_udp.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_sandbox.h"
#include "../net/aether_net.h"
#include "../string/aether_string.h"

#include <stdlib.h>
#include <string.h>

#if !AETHER_HAS_NETWORKING
UdpSocket* udp_bind_raw(const char* h, int p) { (void)h; (void)p; return NULL; }
int udp_local_port_raw(UdpSocket* s) { (void)s; return -1; }
int udp_close(UdpSocket* s) { (void)s; return -1; }
int udp_fd_raw(UdpSocket* s) { (void)s; return -1; }
int udp_poll_raw(UdpSocket* s, int t) { (void)s; (void)t; return -1; }
int udp_set_buffer_sizes_raw(UdpSocket* s, int a, int b) { (void)s; (void)a; (void)b; return -1; }
int udp_set_broadcast_raw(UdpSocket* s, int on) { (void)s; (void)on; return -1; }
UdpAddr* udp_resolve_raw(const char* h, int p) { (void)h; (void)p; return NULL; }
UdpAddr* udp_addr_new(void) { return NULL; }
UdpAddr* udp_addr_clone_raw(const UdpAddr* a) { (void)a; return NULL; }
int udp_addr_free(UdpAddr* a) { (void)a; return -1; }
void* udp_addr_host_raw(const UdpAddr* a) { (void)a; return NULL; }
int udp_addr_port_raw(const UdpAddr* a) { (void)a; return -1; }
int udp_addr_equal_raw(const UdpAddr* a, const UdpAddr* b) { (void)a; (void)b; return 0; }
int udp_addr_hash_raw(const UdpAddr* a) { (void)a; return 0; }
int udp_send_to_raw(UdpSocket* s, const char* h, int p, const void* d, int n) {
    (void)s; (void)h; (void)p; (void)d; (void)n; return -1;
}
int udp_send_to_addr_raw(UdpSocket* s, const UdpAddr* a, const void* d, int n) {
    (void)s; (void)a; (void)d; (void)n; return -1;
}
int udp_recv_from_into_raw(UdpSocket* s, void* b, int c, UdpAddr* a) {
    (void)s; (void)b; (void)c; (void)a; return UDP_RECV_ERROR;
}
UdpRecvResult udp_recv_from_raw(UdpSocket* s, void* b, int c) {
    (void)s; (void)b; (void)c;
    UdpRecvResult out = { 0, NULL, "net unavailable" };
    return out;
}
#else

#include <stdio.h>   /* snprintf: the port string getaddrinfo wants */

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define close closesocket
    #define AE_POLLFD  WSAPOLLFD
    #define ae_poll    WSAPoll
    #define ae_nfds_t  ULONG
    typedef int ae_socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <poll.h>
    #include <fcntl.h>
    #define AE_POLLFD  struct pollfd
    #define ae_poll    poll
    #define ae_nfds_t  nfds_t
    typedef socklen_t ae_socklen_t;
#endif

struct UdpSocket {
    int fd;
    int family;   /* AF_INET or AF_INET6: the family a peer must be in */
    int port;     /* the bound port, read back after a bind on 0 */
};

/* A resolved peer. `len` is 0 for an address nothing has filled yet. */
struct UdpAddr {
    struct sockaddr_storage ss;
    ae_socklen_t len;
};

static int udp_set_nonblocking(int fd) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/* getaddrinfo for a datagram endpoint. `passive` picks the wildcard for
 * an empty host, as a bind wants; a send wants a real destination. Fills
 * `out` with the first result and returns 0, or -1. */
static int udp_lookup(const char* host, int port, int passive, struct UdpAddr* out) {
    if (port < 0 || port > 65535) return -1;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (passive) hints.ai_flags = AI_PASSIVE;
    const char* node = (host && host[0]) ? host : NULL;
    if (!node && !passive) return -1;
    struct addrinfo* res = NULL;
    if (getaddrinfo(node, port_str, &hints, &res) != 0 || !res) return -1;
    if (res->ai_addrlen > sizeof(out->ss)) { freeaddrinfo(res); return -1; }
    memset(&out->ss, 0, sizeof(out->ss));
    memcpy(&out->ss, res->ai_addr, res->ai_addrlen);
    out->len = (ae_socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int udp_port_of(const struct UdpAddr* a) {
    if (a->ss.ss_family == AF_INET) {
        return (int)ntohs(((const struct sockaddr_in*)&a->ss)->sin_port);
    }
    if (a->ss.ss_family == AF_INET6) {
        return (int)ntohs(((const struct sockaddr_in6*)&a->ss)->sin6_port);
    }
    return -1;
}

/* The address bytes that identify a peer, family-agnostic. Returns the
 * byte count, 0 for an unknown family. */
static int udp_addr_bytes(const struct UdpAddr* a, const unsigned char** bytes) {
    if (a->ss.ss_family == AF_INET) {
        *bytes = (const unsigned char*)&((const struct sockaddr_in*)&a->ss)->sin_addr;
        return 4;
    }
    if (a->ss.ss_family == AF_INET6) {
        *bytes = (const unsigned char*)&((const struct sockaddr_in6*)&a->ss)->sin6_addr;
        return 16;
    }
    *bytes = NULL;
    return 0;
}

UdpSocket* udp_bind_raw(const char* host, int port) {
    const char* checked = (host && host[0]) ? host : "0.0.0.0";
    if (!aether_sandbox_check("udp", checked)) return NULL;
    aether_net_init();

    struct UdpAddr local;
    if (udp_lookup(host, port, 1, &local) != 0) return NULL;

    int fd = (int)socket(local.ss.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return NULL;

#ifdef IPV6_V6ONLY
    if (local.ss.ss_family == AF_INET6) {
        /* Dual-stack when bound to "::" so one socket serves both families;
         * best effort, some OSes pin it the other way. */
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
    }
#endif

    if (bind(fd, (struct sockaddr*)&local.ss, local.len) < 0) {
        close(fd);
        return NULL;
    }
    if (udp_set_nonblocking(fd) != 0) {
        close(fd);
        return NULL;
    }

    struct UdpAddr got;
    memset(&got, 0, sizeof(got));
    got.len = (ae_socklen_t)sizeof(got.ss);
    int bound = port;
    if (getsockname(fd, (struct sockaddr*)&got.ss, &got.len) == 0) {
        bound = udp_port_of(&got);
    }

    UdpSocket* sock = (UdpSocket*)malloc(sizeof(UdpSocket));
    if (!sock) { close(fd); return NULL; }
    sock->fd = fd;
    sock->family = local.ss.ss_family;
    sock->port = bound;
    return sock;
}

int udp_local_port_raw(UdpSocket* sock) {
    return sock ? sock->port : -1;
}

int udp_close(UdpSocket* sock) {
    if (!sock) return -1;
    close(sock->fd);
    free(sock);
    return 0;
}

int udp_fd_raw(UdpSocket* sock) {
    return sock ? sock->fd : -1;
}

int udp_poll_raw(UdpSocket* sock, int timeout_ms) {
    if (!sock) return -1;
    AE_POLLFD pfd;
    pfd.fd = sock->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = ae_poll(&pfd, (ae_nfds_t)1, timeout_ms);
    if (rc < 0) {
        if (aether_net_wouldblock()) return 0;   /* EINTR: no event */
        return -1;
    }
    if (rc == 0) return 0;
    return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
}

int udp_set_buffer_sizes_raw(UdpSocket* sock, int send_bytes, int recv_bytes) {
    if (!sock || send_bytes < 0 || recv_bytes < 0) return -1;
    if (send_bytes > 0 &&
        setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, (const char*)&send_bytes, sizeof(send_bytes)) != 0) {
        return -1;
    }
    if (recv_bytes > 0 &&
        setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, (const char*)&recv_bytes, sizeof(recv_bytes)) != 0) {
        return -1;
    }
    return 0;
}

int udp_set_broadcast_raw(UdpSocket* sock, int on) {
    if (!sock) return -1;
    int flag = on ? 1 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_BROADCAST, (const char*)&flag, sizeof(flag)) != 0) {
        return -1;
    }
    return 0;
}

UdpAddr* udp_addr_new(void) {
    UdpAddr* a = (UdpAddr*)calloc(1, sizeof(UdpAddr));
    return a;
}

UdpAddr* udp_resolve_raw(const char* host, int port) {
    if (!host || !host[0]) return NULL;
    if (!aether_sandbox_check("udp", host)) return NULL;
    aether_net_init();
    UdpAddr* a = udp_addr_new();
    if (!a) return NULL;
    if (udp_lookup(host, port, 0, a) != 0) {
        free(a);
        return NULL;
    }
    return a;
}

UdpAddr* udp_addr_clone_raw(const UdpAddr* addr) {
    if (!addr) return NULL;
    UdpAddr* a = (UdpAddr*)malloc(sizeof(UdpAddr));
    if (!a) return NULL;
    memcpy(a, addr, sizeof(UdpAddr));
    return a;
}

int udp_addr_free(UdpAddr* addr) {
    if (!addr) return -1;
    free(addr);
    return 0;
}

void* udp_addr_host_raw(const UdpAddr* addr) {
    if (!addr || addr->len == 0) return NULL;
    char text[INET6_ADDRSTRLEN + 1];
    const void* src = NULL;
    if (addr->ss.ss_family == AF_INET) {
        src = &((const struct sockaddr_in*)&addr->ss)->sin_addr;
    } else if (addr->ss.ss_family == AF_INET6) {
        src = &((const struct sockaddr_in6*)&addr->ss)->sin6_addr;
    } else {
        return NULL;
    }
    if (!inet_ntop(addr->ss.ss_family, (void*)src, text, sizeof(text))) return NULL;
    return (void*)string_new_with_length(text, strlen(text));
}

int udp_addr_port_raw(const UdpAddr* addr) {
    if (!addr || addr->len == 0) return -1;
    return udp_port_of(addr);
}

int udp_addr_equal_raw(const UdpAddr* a, const UdpAddr* b) {
    if (!a || !b || a->len == 0 || b->len == 0) return 0;
    if (a->ss.ss_family != b->ss.ss_family) return 0;
    if (udp_port_of(a) != udp_port_of(b)) return 0;
    const unsigned char* ab; const unsigned char* bb;
    int n = udp_addr_bytes(a, &ab);
    if (n == 0 || udp_addr_bytes(b, &bb) != n) return 0;
    return memcmp(ab, bb, (size_t)n) == 0;
}

/* FNV-1a over family, port and address bytes, folded to a non-negative
 * int so it can index an Aether array or bucket table directly. */
int udp_addr_hash_raw(const UdpAddr* addr) {
    if (!addr || addr->len == 0) return 0;
    const unsigned char* bytes;
    int n = udp_addr_bytes(addr, &bytes);
    unsigned int h = 2166136261u;
    unsigned int family = (unsigned int)addr->ss.ss_family;
    unsigned int port = (unsigned int)udp_port_of(addr);
    unsigned char head[4] = {
        (unsigned char)(family & 0xff), (unsigned char)(family >> 8),
        (unsigned char)(port & 0xff), (unsigned char)(port >> 8)
    };
    for (int i = 0; i < 4; i++) { h ^= head[i]; h *= 16777619u; }
    for (int i = 0; i < n; i++) { h ^= bytes[i]; h *= 16777619u; }
    return (int)(h & 0x7fffffffu);
}

int udp_send_to_addr_raw(UdpSocket* sock, const UdpAddr* addr, const void* data, int n) {
    if (!sock || !addr || addr->len == 0 || n < 0 || (n > 0 && !data)) return -1;
    int sent = (int)sendto(sock->fd, (const char*)data, (size_t)n, 0,
                           (const struct sockaddr*)&addr->ss, addr->len);
    return sent < 0 ? -1 : sent;
}

int udp_send_to_raw(UdpSocket* sock, const char* host, int port, const void* data, int n) {
    if (!sock) return -1;
    UdpAddr* addr = udp_resolve_raw(host, port);
    if (!addr) return -1;
    int sent = udp_send_to_addr_raw(sock, addr, data, n);
    udp_addr_free(addr);
    return sent;
}

int udp_recv_from_into_raw(UdpSocket* sock, void* buf, int cap, UdpAddr* addr) {
    if (!sock || !buf || cap <= 0) return UDP_RECV_ERROR;
    struct UdpAddr scratch;
    struct UdpAddr* target = addr ? addr : &scratch;
    memset(&target->ss, 0, sizeof(target->ss));
    target->len = (ae_socklen_t)sizeof(target->ss);
    int got = (int)recvfrom(sock->fd, (char*)buf, (size_t)cap, 0,
                            (struct sockaddr*)&target->ss, &target->len);
    if (got < 0) {
        target->len = 0;
        return aether_net_wouldblock() ? UDP_RECV_WOULDBLOCK : UDP_RECV_ERROR;
    }
    return got;
}

UdpRecvResult udp_recv_from_raw(UdpSocket* sock, void* buf, int cap) {
    UdpRecvResult out;
    out._0 = 0;
    out._1 = NULL;
    out._2 = "receive failed";
    UdpAddr* addr = udp_addr_new();
    if (!addr) {
        out._2 = "allocation failed";
        return out;
    }
    int got = udp_recv_from_into_raw(sock, buf, cap, addr);
    if (got < 0) {
        free(addr);
        if (got == UDP_RECV_WOULDBLOCK) out._2 = "would block";
        return out;
    }
    out._0 = got;
    out._1 = addr;
    out._2 = "";
    return out;
}

#endif /* AETHER_HAS_NETWORKING */
