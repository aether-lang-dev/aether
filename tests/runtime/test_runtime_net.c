#include "test_harness.h"
#include "../../std/net/aether_net.h"
#include "../../std/string/aether_string.h"
#include <errno.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

TEST_CATEGORY(socket_null_handling, TEST_CATEGORY_NETWORK) {
    int result = tcp_send_raw(NULL, NULL);
    ASSERT_EQ(-1, result);

    result = tcp_send_n_raw(NULL, NULL, 0);
    ASSERT_EQ(-1, result);

    char* received = tcp_receive_raw(NULL, 1024);
    ASSERT_NULL(received);

    TcpReceiveResult received_n = tcp_receive_n_raw(NULL, 1024);
    ASSERT_NULL(received_n._0);
    ASSERT_EQ(0, received_n._1);
    ASSERT_STRNE("", received_n._2);

    result = tcp_close(NULL);
    ASSERT_EQ(-1, result);
}

TEST_CATEGORY(server_null_handling, TEST_CATEGORY_NETWORK) {
    TcpSocket* sock = tcp_accept_raw(NULL);
    ASSERT_NULL(sock);

    int result = tcp_server_close(NULL);
    ASSERT_EQ(-1, result);
}

TEST_CATEGORY(socket_connect_invalid_host, TEST_CATEGORY_NETWORK) {
#ifndef _WIN32
    TcpSocket* sock = tcp_connect_raw("invalid.host.that.does.not.exist.12345", 80);
    ASSERT_NULL(sock);
#else
    ASSERT_TRUE(1);  // DNS resolution can hang on Windows
#endif
}

TEST_CATEGORY(server_create_invalid_port, TEST_CATEGORY_NETWORK) {
    TcpServer* server = tcp_listen_raw(-1);  /* loopback-ok: port -1 is refused before any bind */
    ASSERT_NULL(server);
}

#ifndef _WIN32
// Mirror the private runtime layout so this unit test can wrap a POSIX
// socketpair without depending on a bindable TCP port.
struct TcpSocket {
    int fd;
    int connected;
};

TEST_CATEGORY(tcp_binary_nul_roundtrip, TEST_CATEGORY_NETWORK) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    TcpSocket left = { fds[0], 1 };
    TcpSocket right = { fds[1], 1 };

    const char payload[3] = { 'A', '\0', 'B' };
    errno = 0;
    int sent = tcp_send_n_raw(&left, payload, 3);
    if (sent < 0) {
        if (errno == EPERM) {
            printf("  SKIP tcp_binary_nul_roundtrip: socket send denied by sandbox\n");
            close(fds[0]);
            close(fds[1]);
            return;
        }
    }
    ASSERT_EQ(3, sent);

    TcpReceiveResult got = tcp_receive_n_raw(&right, 16);
    ASSERT_NOT_NULL(got._0);
    ASSERT_EQ(3, got._1);

    const char* data = aether_string_data(got._0);
    ASSERT_EQ('A', data[0]);
    ASSERT_EQ('\0', data[1]);
    ASSERT_EQ('B', data[2]);
    string_release(got._0);

    close(fds[0]);
    close(fds[1]);
}

// #1092: a recv that would block (nonblocking socket with nothing
// buffered, or SO_RCVTIMEO firing) must be reported as a distinct
// "timeout" and must NOT mark the socket dead. Collapsing it into the
// close branch tears down a quiet-but-alive tunnel direction.
TEST_CATEGORY(tcp_receive_timeout_is_not_fatal, TEST_CATEGORY_NETWORK) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    // Make the read end nonblocking so recv returns EAGAIN immediately
    // rather than waiting on the 30 s SO_RCVTIMEO.
    int flags = fcntl(fds[1], F_GETFL, 0);
    ASSERT_TRUE(flags >= 0);
    ASSERT_EQ(0, fcntl(fds[1], F_SETFL, flags | O_NONBLOCK));

    TcpSocket right = { fds[1], 1 };

    // Nothing sent yet: recv would block. Expect the timeout sentinel and
    // an intact connected flag.
    TcpReceiveResult idle = tcp_receive_n_raw(&right, 16);
    ASSERT_NULL(idle._0);
    ASSERT_EQ(0, idle._1);
    ASSERT_STREQ("timeout", idle._2);
    ASSERT_EQ(1, right.connected);   // still alive — the crux of #1092

    // The same socket must still work: send then read succeeds.
    TcpSocket left = { fds[0], 1 };
    const char payload[3] = { 'x', '\0', 'y' };
    errno = 0;
    int sent = tcp_send_n_raw(&left, payload, 3);
    if (sent < 0 && errno == EPERM) {
        printf("  SKIP tcp_receive_timeout_is_not_fatal: send denied by sandbox\n");
        close(fds[0]); close(fds[1]);
        return;
    }
    ASSERT_EQ(3, sent);

    TcpReceiveResult got = tcp_receive_n_raw(&right, 16);
    ASSERT_NOT_NULL(got._0);
    ASSERT_EQ(3, got._1);
    ASSERT_STREQ("", got._2);
    string_release(got._0);

    // A genuine peer close (FIN) IS fatal: connected goes to 0.
    close(fds[0]);
    TcpReceiveResult closed = tcp_receive_n_raw(&right, 16);
    ASSERT_NULL(closed._0);
    ASSERT_EQ(0, right.connected);

    close(fds[1]);
}

// #1092: readiness primitives. poll reports timeout vs ready without
// reading; poll2 reports which of two sockets is readable as a bitmask.
TEST_CATEGORY(tcp_poll_readiness, TEST_CATEGORY_NETWORK) {
    // Null/closed handles are an error, not a silent timeout.
    ASSERT_EQ(-1, tcp_poll_raw(NULL, 0));
    ASSERT_EQ(-1, tcp_poll2_raw(NULL, NULL, 0));

    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    TcpSocket a = { fds[0], 1 };
    TcpSocket b = { fds[1], 1 };

    // Nothing buffered: poll times out (0) with a short deadline.
    ASSERT_EQ(0, tcp_poll_raw(&b, 50));

    // poll2 with both idle also times out.
    ASSERT_EQ(0, tcp_poll2_raw(&a, &b, 50));

    // Send from a->b: b becomes readable, a does not.
    const char one = 'Z';
    errno = 0;
    int sent = tcp_send_n_raw(&a, &one, 1);
    if (sent < 0 && errno == EPERM) {
        printf("  SKIP tcp_poll_readiness: send denied by sandbox\n");
        close(fds[0]); close(fds[1]);
        return;
    }
    ASSERT_EQ(1, sent);

    ASSERT_EQ(1, tcp_poll_raw(&b, 1000));       // b readable
    ASSERT_EQ(2, tcp_poll2_raw(&a, &b, 1000));  // bit 1 (b) only

    close(fds[0]);
    close(fds[1]);
}
#endif
