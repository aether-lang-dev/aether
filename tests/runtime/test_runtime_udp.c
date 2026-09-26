// std.udp (#2201): datagram sockets over loopback, held to numbers at the
// C layer so the Aether wrappers only have to be checked for their tuple
// shapes (std/udp/test_udp.ae).
#include "test_harness.h"
#include "../../std/udp/aether_udp.h"
#include "../../std/string/aether_string.h"
#include <string.h>

TEST_CATEGORY(udp_null_handles, TEST_CATEGORY_NETWORK) {
    ASSERT_EQ(-1, udp_local_port_raw(NULL));
    ASSERT_EQ(-1, udp_close(NULL));
    ASSERT_EQ(-1, udp_fd_raw(NULL));
    ASSERT_EQ(-1, udp_poll_raw(NULL, 0));
    ASSERT_EQ(-1, udp_set_buffer_sizes_raw(NULL, 1024, 1024));
    ASSERT_EQ(-1, udp_set_broadcast_raw(NULL, 1));
    ASSERT_EQ(-1, udp_addr_free(NULL));
    ASSERT_NULL(udp_addr_clone_raw(NULL));
    ASSERT_NULL(udp_addr_host_raw(NULL));
    ASSERT_EQ(-1, udp_addr_port_raw(NULL));
    ASSERT_EQ(0, udp_addr_equal_raw(NULL, NULL));
    ASSERT_EQ(0, udp_addr_hash_raw(NULL));
    ASSERT_EQ(-1, udp_send_to_raw(NULL, "127.0.0.1", 1, "x", 1));
    ASSERT_EQ(-1, udp_send_to_addr_raw(NULL, NULL, "x", 1));

    char buf[8];
    ASSERT_EQ(UDP_RECV_ERROR, udp_recv_from_into_raw(NULL, buf, sizeof(buf), NULL));
    UdpRecvResult r = udp_recv_from_raw(NULL, buf, sizeof(buf));
    ASSERT_EQ(0, r._0);
    ASSERT_NULL(r._1);
    ASSERT_STRNE("", r._2);
}

TEST_CATEGORY(udp_bind_rejects_bad_input, TEST_CATEGORY_NETWORK) {
    ASSERT_NULL(udp_bind_raw("127.0.0.1", -1));
    ASSERT_NULL(udp_bind_raw("127.0.0.1", 65536));
    ASSERT_NULL(udp_bind_raw("not an address", 0));
    ASSERT_NULL(udp_resolve_raw("", 5));
    ASSERT_NULL(udp_resolve_raw(NULL, 5));
    ASSERT_NULL(udp_resolve_raw("127.0.0.1", 70000));
}

TEST_CATEGORY(udp_bind_ephemeral_port_is_read_back, TEST_CATEGORY_NETWORK) {
    UdpSocket* s = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(s);
    int port = udp_local_port_raw(s);
    ASSERT_TRUE(port > 0 && port <= 65535);
    ASSERT_TRUE(udp_fd_raw(s) >= 0);
    ASSERT_EQ(0, udp_close(s));
}

TEST_CATEGORY(udp_empty_host_binds_every_interface, TEST_CATEGORY_NETWORK) {
    UdpSocket* s = udp_bind_raw("", 0);
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(udp_local_port_raw(s) > 0);
    udp_close(s);
}

TEST_CATEGORY(udp_recv_on_idle_socket_would_block, TEST_CATEGORY_NETWORK) {
    UdpSocket* s = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(s);
    char buf[16];
    ASSERT_EQ(UDP_RECV_WOULDBLOCK, udp_recv_from_into_raw(s, buf, sizeof(buf), NULL));
    UdpRecvResult r = udp_recv_from_raw(s, buf, sizeof(buf));
    ASSERT_EQ(0, r._0);
    ASSERT_NULL(r._1);
    ASSERT_STREQ("would block", r._2);
    ASSERT_EQ(0, udp_poll_raw(s, 0));
    udp_close(s);
}

TEST_CATEGORY(udp_loopback_roundtrip_by_host, TEST_CATEGORY_NETWORK) {
    UdpSocket* server = udp_bind_raw("127.0.0.1", 0);
    UdpSocket* client = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(server);
    ASSERT_NOT_NULL(client);

    const char payload[] = "snap\0shot";   /* an embedded NUL survives */
    int sent = udp_send_to_raw(client, "127.0.0.1", udp_local_port_raw(server),
                               payload, (int)sizeof(payload));
    ASSERT_EQ((int)sizeof(payload), sent);

    ASSERT_EQ(1, udp_poll_raw(server, 1000));
    char buf[64];
    UdpRecvResult r = udp_recv_from_raw(server, buf, sizeof(buf));
    ASSERT_STREQ("", r._2);
    ASSERT_EQ((int)sizeof(payload), r._0);
    ASSERT_EQ(0, memcmp(payload, buf, sizeof(payload)));
    ASSERT_NOT_NULL(r._1);

    UdpAddr* sender = (UdpAddr*)r._1;
    ASSERT_EQ(udp_local_port_raw(client), udp_addr_port_raw(sender));
    AetherString* host = (AetherString*)udp_addr_host_raw(sender);
    ASSERT_NOT_NULL(host);
    ASSERT_STREQ("127.0.0.1", string_to_cstr(host));
    string_release(host);

    /* Answer the sender through its address: no resolver involved. */
    ASSERT_EQ(3, udp_send_to_addr_raw(server, sender, "ack", 3));
    ASSERT_EQ(1, udp_poll_raw(client, 1000));
    UdpAddr* from = udp_addr_new();
    ASSERT_EQ(3, udp_recv_from_into_raw(client, buf, sizeof(buf), from));
    ASSERT_EQ(0, memcmp("ack", buf, 3));
    ASSERT_EQ(udp_local_port_raw(server), udp_addr_port_raw(from));

    udp_addr_free(from);
    udp_addr_free(sender);
    udp_close(client);
    udp_close(server);
}

TEST_CATEGORY(udp_recv_truncates_to_capacity, TEST_CATEGORY_NETWORK) {
    UdpSocket* server = udp_bind_raw("127.0.0.1", 0);
    UdpSocket* client = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(server);
    ASSERT_NOT_NULL(client);
    ASSERT_EQ(10, udp_send_to_raw(client, "127.0.0.1", udp_local_port_raw(server), "0123456789", 10));
    ASSERT_EQ(1, udp_poll_raw(server, 1000));
    char buf[4];
    ASSERT_EQ(4, udp_recv_from_into_raw(server, buf, sizeof(buf), NULL));
    ASSERT_EQ(0, memcmp("0123", buf, 4));
    /* The rest of that datagram is gone, not queued. */
    ASSERT_EQ(UDP_RECV_WOULDBLOCK, udp_recv_from_into_raw(server, buf, sizeof(buf), NULL));
    udp_close(client);
    udp_close(server);
}

TEST_CATEGORY(udp_zero_length_datagram_is_a_packet, TEST_CATEGORY_NETWORK) {
    UdpSocket* server = udp_bind_raw("127.0.0.1", 0);
    UdpSocket* client = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(server);
    ASSERT_NOT_NULL(client);
    UdpAddr* to = udp_resolve_raw("127.0.0.1", udp_local_port_raw(server));
    ASSERT_NOT_NULL(to);
    ASSERT_EQ(0, udp_send_to_addr_raw(client, to, NULL, 0));
    ASSERT_EQ(1, udp_poll_raw(server, 1000));
    char buf[4];
    UdpRecvResult r = udp_recv_from_raw(server, buf, sizeof(buf));
    ASSERT_STREQ("", r._2);
    ASSERT_EQ(0, r._0);
    ASSERT_NOT_NULL(r._1);   /* a sender: this was a packet, not "would block" */
    udp_addr_free((UdpAddr*)r._1);
    udp_addr_free(to);
    udp_close(client);
    udp_close(server);
}

TEST_CATEGORY(udp_send_rejects_bad_arguments, TEST_CATEGORY_NETWORK) {
    UdpSocket* s = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(s);
    UdpAddr* to = udp_resolve_raw("127.0.0.1", udp_local_port_raw(s));
    ASSERT_NOT_NULL(to);
    ASSERT_EQ(-1, udp_send_to_addr_raw(s, to, NULL, 3));       /* data missing */
    ASSERT_EQ(-1, udp_send_to_addr_raw(s, to, "abc", -1));     /* negative length */
    UdpAddr* unset = udp_addr_new();
    ASSERT_EQ(-1, udp_send_to_addr_raw(s, unset, "abc", 3));   /* never resolved */
    ASSERT_EQ(-1, udp_send_to_raw(s, "no.such.host.invalid", 1, "abc", 3));
    char buf[4];
    ASSERT_EQ(UDP_RECV_ERROR, udp_recv_from_into_raw(s, NULL, 4, NULL));
    ASSERT_EQ(UDP_RECV_ERROR, udp_recv_from_into_raw(s, buf, 0, NULL));
    udp_addr_free(unset);
    udp_addr_free(to);
    udp_close(s);
}

TEST_CATEGORY(udp_addr_value_semantics, TEST_CATEGORY_NETWORK) {
    UdpAddr* a = udp_resolve_raw("127.0.0.1", 7000);
    UdpAddr* same = udp_resolve_raw("127.0.0.1", 7000);
    UdpAddr* other_port = udp_resolve_raw("127.0.0.1", 7001);
    UdpAddr* other_host = udp_resolve_raw("127.0.0.2", 7000);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(same);
    ASSERT_NOT_NULL(other_port);
    ASSERT_NOT_NULL(other_host);

    ASSERT_EQ(7000, udp_addr_port_raw(a));
    ASSERT_EQ(1, udp_addr_equal_raw(a, same));
    ASSERT_EQ(1, udp_addr_equal_raw(same, a));
    ASSERT_EQ(0, udp_addr_equal_raw(a, other_port));
    ASSERT_EQ(0, udp_addr_equal_raw(a, other_host));

    ASSERT_EQ(udp_addr_hash_raw(a), udp_addr_hash_raw(same));
    ASSERT_TRUE(udp_addr_hash_raw(a) >= 0);
    ASSERT_NE(udp_addr_hash_raw(a), udp_addr_hash_raw(other_port));
    ASSERT_NE(udp_addr_hash_raw(a), udp_addr_hash_raw(other_host));

    UdpAddr* copy = udp_addr_clone_raw(a);
    ASSERT_NOT_NULL(copy);
    ASSERT_EQ(1, udp_addr_equal_raw(a, copy));
    ASSERT_EQ(udp_addr_hash_raw(a), udp_addr_hash_raw(copy));

    /* An unset address is nobody: not equal to anything, not even itself. */
    UdpAddr* unset = udp_addr_new();
    ASSERT_NOT_NULL(unset);
    ASSERT_EQ(0, udp_addr_equal_raw(unset, unset));
    ASSERT_EQ(0, udp_addr_equal_raw(a, unset));
    ASSERT_EQ(0, udp_addr_hash_raw(unset));
    ASSERT_EQ(-1, udp_addr_port_raw(unset));
    ASSERT_NULL(udp_addr_host_raw(unset));

    udp_addr_free(unset);
    udp_addr_free(copy);
    udp_addr_free(other_host);
    udp_addr_free(other_port);
    udp_addr_free(same);
    udp_addr_free(a);
}

TEST_CATEGORY(udp_socket_options, TEST_CATEGORY_NETWORK) {
    UdpSocket* s = udp_bind_raw("127.0.0.1", 0);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(0, udp_set_buffer_sizes_raw(s, 65536, 65536));
    ASSERT_EQ(0, udp_set_buffer_sizes_raw(s, 0, 0));          /* leave both alone */
    ASSERT_EQ(-1, udp_set_buffer_sizes_raw(s, -1, 0));
    ASSERT_EQ(0, udp_set_broadcast_raw(s, 1));
    ASSERT_EQ(0, udp_set_broadcast_raw(s, 0));
    udp_close(s);
}

TEST_CATEGORY(udp_ipv6_loopback_roundtrip, TEST_CATEGORY_NETWORK) {
    UdpSocket* server = udp_bind_raw("::1", 0);
    if (!server) {
        ASSERT_TRUE(1);   /* no IPv6 loopback on this host: nothing to hold */
        return;
    }
    UdpSocket* client = udp_bind_raw("::1", 0);
    ASSERT_NOT_NULL(client);
    UdpAddr* to = udp_resolve_raw("::1", udp_local_port_raw(server));
    ASSERT_NOT_NULL(to);
    ASSERT_EQ(2, udp_send_to_addr_raw(client, to, "v6", 2));
    ASSERT_EQ(1, udp_poll_raw(server, 1000));
    char buf[8];
    UdpRecvResult r = udp_recv_from_raw(server, buf, sizeof(buf));
    ASSERT_STREQ("", r._2);
    ASSERT_EQ(2, r._0);
    AetherString* host = (AetherString*)udp_addr_host_raw((UdpAddr*)r._1);
    ASSERT_STREQ("::1", string_to_cstr(host));
    string_release(host);
    /* A v4 and a v6 address never compare equal, whatever the port. */
    UdpAddr* v4 = udp_resolve_raw("127.0.0.1", udp_local_port_raw(server));
    ASSERT_NOT_NULL(v4);
    ASSERT_EQ(0, udp_addr_equal_raw(to, v4));
    udp_addr_free(v4);
    udp_addr_free((UdpAddr*)r._1);
    udp_addr_free(to);
    udp_close(client);
    udp_close(server);
}
