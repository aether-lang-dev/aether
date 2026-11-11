/* Names under localhost. are loopback, asked of no resolver (RFC 6761 6.3):
 * "app.localhost" failed to resolve on Windows and FreeBSD, and on FreeBSD
 * and a glibc host without systemd-resolved's stub it went to the DNS
 * server. std.net's dial and listen, the HTTP client and the WebSocket dial
 * all resolve through aether_net_addr.h. */
#include "test_harness.h"
#include <string.h>
#include "../../std/net/aether_net_addr.h"

static int is_v6_loopback(const struct sockaddr_storage* ss, int port) {
    const struct sockaddr_in6* a = (const struct sockaddr_in6*)ss;
    static const unsigned char lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    return ss->ss_family == AF_INET6 && memcmp(&a->sin6_addr, lo, 16) == 0
        && ntohs(a->sin6_port) == port;
}

static int is_v4_loopback(const struct sockaddr_storage* ss, int port) {
    const struct sockaddr_in* a = (const struct sockaddr_in*)ss;
    return ss->ss_family == AF_INET && ntohl(a->sin_addr.s_addr) == INADDR_LOOPBACK
        && ntohs(a->sin_port) == port;
}

/* Which names are localhost. ones: a label (or several) before
 * ".localhost", in any case, with or without the root dot, and the rooted
 * "localhost." itself. Plain "localhost" is left to the hosts file, and a
 * name merely containing or ending in the letters is not one. */
TEST(net_localhost_names) {
    ASSERT_TRUE(aether_addr_is_localhost_name("app.localhost"));
    ASSERT_TRUE(aether_addr_is_localhost_name("a.b.localhost"));
    ASSERT_TRUE(aether_addr_is_localhost_name("API.LocalHost"));
    ASSERT_TRUE(aether_addr_is_localhost_name("app.localhost."));
    ASSERT_TRUE(aether_addr_is_localhost_name("localhost."));
    ASSERT_FALSE(aether_addr_is_localhost_name("localhost"));
    ASSERT_FALSE(aether_addr_is_localhost_name(".localhost"));
    ASSERT_FALSE(aether_addr_is_localhost_name("notlocalhost"));
    ASSERT_FALSE(aether_addr_is_localhost_name("app-localhost"));
    ASSERT_FALSE(aether_addr_is_localhost_name("localhost.example"));
    ASSERT_FALSE(aether_addr_is_localhost_name("app.localhost.example"));
    ASSERT_FALSE(aether_addr_is_localhost_name("app.localhost.."));
    ASSERT_FALSE(aether_addr_is_localhost_name(""));
    ASSERT_FALSE(aether_addr_is_localhost_name("."));
}

/* A dial to one gets ::1 then 127.0.0.1 with the port, on every platform;
 * brackets are no part of a name, so "[app.localhost]" is not one. */
TEST(net_localhost_resolves_to_loopback) {
    AetherAddrList a;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const char* names[] = { "app.localhost", "A.B.LOCALHOST", "app.localhost.", "localhost." };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        ASSERT_EQ(0, aether_addr_resolve(names[i], 8443, &a));
        ASSERT_EQ(2, a.n);
        ASSERT_TRUE(is_v6_loopback(&a.ss[0], 8443));
        ASSERT_EQ((int)sizeof(struct sockaddr_in6), (int)a.len[0]);
        ASSERT_TRUE(is_v4_loopback(&a.ss[1], 8443));
        ASSERT_EQ((int)sizeof(struct sockaddr_in), (int)a.len[1]);
    }
}

/* A listener on one binds 127.0.0.1, as a listener on "localhost" does. */
TEST(net_localhost_binds_ipv4_loopback) {
    struct sockaddr_storage ss;
    socklen_t len = 0;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    ASSERT_EQ(0, aether_addr_bind("api.localhost", 9001, &ss, &len));
    ASSERT_TRUE(is_v4_loopback(&ss, 9001));
    ASSERT_EQ((int)sizeof(struct sockaddr_in), (int)len);
}
