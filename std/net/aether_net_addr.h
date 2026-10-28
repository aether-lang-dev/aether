/* Address resolution shared by std.net's TCP calls, the HTTP client and the
 * HTTP server, for both families.
 *
 * A host is spelled the way a URL authority or a sandbox resource spells it:
 * a name ("localhost"), a dotted IPv4 address, or an IPv6 address with or
 * without brackets ("::1" or "[::1]"). Everything here takes either form and
 * hands getaddrinfo the bare one.
 *
 * Dialling gets every address a name resolves to, in getaddrinfo's order
 * (RFC 6724: IPv6 first where the host has it), so the caller can try each in
 * turn. Binding keeps the old IPv4 answer for a name, so "localhost" listens
 * on 127.0.0.1 exactly as before; "::" listens on both families.
 *
 * Header-only: the three users compile in separate translation units and
 * there is nothing here worth a symbol of its own. */
#ifndef AETHER_NET_ADDR_H
#define AETHER_NET_ADDR_H

#include <string.h>
#include <stdio.h>
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#endif

#define AETHER_ADDR_MAX 8

typedef struct AetherAddrList {
    struct sockaddr_storage ss[AETHER_ADDR_MAX];
    socklen_t len[AETHER_ADDR_MAX];
    int n;
} AetherAddrList;

/* `host` without the brackets of an IPv6 literal, in `buf`. A host that
 * opens a bracket and doesn't close it, or doesn't fit, gives NULL. */
static inline const char* aether_addr_unbracket(const char* host, char* buf, size_t cap) {
    if (!host) return NULL;
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[') {
        if (host[n - 1] != ']') return NULL;
        host++;
        n -= 2;
    } else if (host[0] == '[') {
        return NULL;
    }
    if (n >= cap) return NULL;
    memcpy(buf, host, n);
    buf[n] = '\0';
    return buf;
}

/* `host:port` with brackets around an IPv6 host, as a URL authority, a Host
 * header and a CONNECT line write it. A port below 0 is left off. */
static inline const char* aether_addr_authority(char* buf, size_t cap, const char* host, int port) {
    if (!host) host = "";
    int v6 = strchr(host, ':') && host[0] != '[';
    if (port < 0) snprintf(buf, cap, v6 ? "[%s]" : "%s", host);
    else snprintf(buf, cap, v6 ? "[%s]:%d" : "%s:%d", host, port);
    return buf;
}

/* A URL authority `a` of `alen` bytes split into its host, without the
 * brackets of an IPv6 literal ("[::1]:8443" gives "::1"), and its port text,
 * *port_at NULL when there is none. An unclosed bracket, anything but a port
 * after the closing one, or a host that doesn't fit gives -1; else 0. */
static inline int aether_addr_split_authority(const char* a, size_t alen, char* host, size_t cap,
                                              const char** port_at, size_t* port_len) {
    const char* end = a + alen;
    const char* host_at = a;
    const char* host_end;
    const char* colon = NULL;
    *port_at = NULL;
    *port_len = 0;
    if (alen > 0 && a[0] == '[') {
        const char* close = memchr(a, ']', alen);
        if (!close) return -1;
        host_at = a + 1;
        host_end = close;
        if (close + 1 < end) {
            if (close[1] != ':') return -1;
            colon = close + 1;
        }
    } else {
        colon = memchr(a, ':', alen);
        host_end = colon ? colon : end;
    }
    size_t hlen = (size_t)(host_end - host_at);
    if (hlen >= cap) return -1;
    memcpy(host, host_at, hlen);
    host[hlen] = '\0';
    if (colon) {
        *port_at = colon + 1;
        *port_len = (size_t)(end - colon - 1);
    }
    return 0;
}

/* Every address `host` resolves to, port filled in, for dialling. Returns 0;
 * -2 when the resolver says the name does not exist (EAI_NONAME, or
 * EAI_NODATA where there is one), which a cache drops the name for; or -1
 * for any other failure, a resolver that did not answer among them. */
static inline int aether_addr_resolve(const char* host, int port, AetherAddrList* out) {
    char bare[256];
    out->n = 0;
    if (!aether_addr_unbracket(host, bare, sizeof bare) || !bare[0]) return -1;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(bare, port_str, &hints, &res);
    if (rc == EAI_NONAME) return -2;
#ifdef EAI_NODATA
    if (rc == EAI_NODATA) return -2;
#endif
    if (rc != 0 || !res) return -1;
    for (struct addrinfo* r = res; r && out->n < AETHER_ADDR_MAX; r = r->ai_next) {
        if (r->ai_family != AF_INET && r->ai_family != AF_INET6) continue;
        if ((size_t)r->ai_addrlen > sizeof out->ss[0]) continue;
        memset(&out->ss[out->n], 0, sizeof out->ss[0]);
        memcpy(&out->ss[out->n], r->ai_addr, r->ai_addrlen);
        out->len[out->n] = (socklen_t)r->ai_addrlen;
        out->n++;
    }
    freeaddrinfo(res);
    return out->n > 0 ? 0 : -1;
}

/* The one address a listener on `host` binds. NULL, "" and "0.0.0.0" are
 * every IPv4 interface; "::" (or "[::]") is every interface of both families,
 * which the caller makes dual-stack with aether_addr_listen_socket; a literal
 * is that address; a name is the IPv4 address it resolves to, or its IPv6
 * one when it has none. A name is never "every interface": that was #2639.
 * Returns 0, or -1 for a host that does not resolve. */
static inline int aether_addr_bind(const char* host, int port, struct sockaddr_storage* out, socklen_t* len) {
    memset(out, 0, sizeof *out);
    if (!host || !host[0] || strcmp(host, "0.0.0.0") == 0) {
        struct sockaddr_in* a = (struct sockaddr_in*)out;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_ANY);
        a->sin_port = htons((unsigned short)port);
        *len = sizeof *a;
        return 0;
    }
    char bare[256];
    if (!aether_addr_unbracket(host, bare, sizeof bare) || !bare[0]) return -1;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    static const int families[2] = { AF_INET, AF_INET6 };
    for (int i = 0; i < 2; i++) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family   = families[i];
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags    = AI_PASSIVE;
        if (getaddrinfo(bare, port_str, &hints, &res) == 0 && res) {
            if ((size_t)res->ai_addrlen <= sizeof *out) {
                memcpy(out, res->ai_addr, res->ai_addrlen);
                *len = (socklen_t)res->ai_addrlen;
                freeaddrinfo(res);
                return 0;
            }
            freeaddrinfo(res);
        }
    }
    return -1;
}

/* A listening-ready socket for `addr`: rebindable while an old connection
 * sits in TIME_WAIT, but never shared with a live listener, and IPV6_V6ONLY
 * off for the IPv6 wildcard, so "::" takes IPv4 clients too (as v4-mapped
 * peers). Platforms differ on the default; Windows and the BSDs default to
 * v6-only, so it is set explicitly. Returns the descriptor, or -1.
 *
 * On POSIX that is SO_REUSEADDR. On Windows SO_REUSEADDR means something
 * else: it lets a second socket bind a port another process is listening on,
 * so a server started twice would "succeed" and split the traffic. Windows
 * rebinds past TIME_WAIT without help, so it gets SO_EXCLUSIVEADDRUSE, and a
 * taken port fails the bind as it does everywhere else. */
static inline int aether_addr_listen_socket(const struct sockaddr_storage* addr) {
    int fd = (int)socket(addr->ss_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&opt, sizeof opt);
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof opt);
#endif
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)addr;
        static const unsigned char any[16] = {0};
        int v6only = memcmp(&a6->sin6_addr, any, 16) != 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof v6only);
    }
    return fd;
}

/* An IPv4 peer on a dual-stack socket arrives as an IPv4-mapped IPv6
 * address (::ffff:192.0.2.1). Rewrite it in place as the IPv4 address it
 * is, so it is reported, logged and matched against IPv4 allowlists in the
 * form everyone writes it. Anything else is left alone. */
static inline void aether_addr_unmap(struct sockaddr_storage* addr) {
    if (addr->ss_family != AF_INET6) return;
    const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)addr;
    static const unsigned char prefix[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    if (memcmp(&a6->sin6_addr, prefix, 12) != 0) return;
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof a4);
    a4.sin_family = AF_INET;
    a4.sin_port = a6->sin6_port;
    memcpy(&a4.sin_addr, (const unsigned char*)&a6->sin6_addr + 12, 4);
    memset(addr, 0, sizeof *addr);
    memcpy(addr, &a4, sizeof a4);
}

/* The port of an IPv4 or IPv6 address, 0 for anything else. */
static inline int aether_addr_port(const struct sockaddr_storage* addr) {
    if (addr->ss_family == AF_INET) return ntohs(((const struct sockaddr_in*)addr)->sin_port);
    if (addr->ss_family == AF_INET6) return ntohs(((const struct sockaddr_in6*)addr)->sin6_port);
    return 0;
}

/* Dialling, in std/net/aether_net.c (one copy, shared by std.net, the HTTP
 * client, its proxy and the WebSocket dial).
 *
 * A name with more than one address is dialled the way RFC 8305 ("Happy
 * Eyeballs") intends, sequentially: every address but the last gets a short
 * first attempt (AETHER_DIAL_FIRST_TRY_MS), the last gets the rest of the
 * budget, and an address that only timed out gets a second attempt after. An
 * IPv6 address that refuses slowly -- Windows retries a refused SYN for about
 * two seconds, so "localhost" resolving to ::1 first costs that on every
 * connection to a server listening on 127.0.0.1 -- no longer sets the pace.
 *
 * The family that connected is remembered per host and port, and tried first
 * next time with the whole budget, so the cost is paid once rather than per
 * connection, and a proven route is never cut short. */
#define AETHER_DIAL_FIRST_TRY_MS 250

/* Connect a fresh socket to `addr`: within `timeout_ns` when positive, else
 * blocking. `read_buffer` > 0 is the socket's SO_RCVBUF, set before the
 * connection is made (0 leaves the kernel's). Returns the descriptor in
 * blocking mode, or -1 with *why set and *timed_out 1 when the attempt ran
 * out of time rather than failed. */
int aether_net_connect_addr(const struct sockaddr_storage* addr, socklen_t len,
                            long long timeout_ns, int read_buffer,
                            const char** why, int* timed_out);
/* Dial `host`:`port` over the already-resolved `addrs` (reordered in place
 * by the remembered family). `timeout_ns` <= 0 means no limit; `read_buffer`
 * as aether_net_connect_addr. */
int aether_net_dial_addrs(const char* host, int port, AetherAddrList* addrs,
                          long long timeout_ns, int read_buffer, const char** why);
/* Resolve, then aether_net_dial_addrs. */
int aether_net_dial(const char* host, int port, long long timeout_ns, const char** why);
/* The remembered family: put its addresses first. Returns that family, or 0
 * when none is remembered. */
int  aether_net_order(const char* host, int port, AetherAddrList* addrs);
/* Remember that `family` connected to `host`:`port`. */
void aether_net_note_family(const char* host, int port, int family);

#endif // AETHER_NET_ADDR_H
