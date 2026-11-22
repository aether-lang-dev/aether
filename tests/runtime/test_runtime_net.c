#include "test_harness.h"
#include "../../std/net/aether_net.h"
#include "../../std/string/aether_string.h"
#include <errno.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
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
// socketpair without depending on a bindable TCP port. Keep it the whole
// struct (std/net/aether_net.c): a brace initialiser zeroes the fields it
// does not name, so no deadline and no cancel handle are bound.
struct TcpSocket {
    int fd;
    int connected;
    long long due_ms;
    void* cancel;
    int stop;
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
/* #3108: a send against a peer that reads slowly can take fewer bytes than
 * asked. With a small send buffer, a 500 ms send timeout and nobody
 * reading, one send of 256 KiB comes back short. A reader that takes 64 KiB
 * and then pauses 750 ms runs a send into its timeout after part of the
 * bytes went; tcp_send_all_raw sends the rest when the reader goes on (well
 * inside the next send's timeout), and every byte arrives in order. */
typedef struct {
    int fd;
    unsigned char* buf;
    int len;
    int got;
} SlowReader;

static void* slow_reader(void* arg) {
    SlowReader* r = (SlowReader*)arg;
    int paused = 0;
    while (r->got < r->len) {
        int want = r->len - r->got < 4096 ? r->len - r->got : 4096;
        int n = (int)recv(r->fd, r->buf + r->got, (size_t)want, 0);
        if (n <= 0) break;
        r->got += n;
        if (!paused && r->got >= 64 * 1024) {
            paused = 1;
            usleep(750000);
        }
    }
    return NULL;
}

static void shrink_sender(int send_fd, int recv_fd) {
    int small = 4096;
    struct timeval tv = { 0, 500000 };
    setsockopt(send_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(recv_fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    setsockopt(send_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

TEST_CATEGORY(tcp_send_all_delivers_through_short_sends, TEST_CATEGORY_NETWORK) {
    enum { LEN = 256 * 1024 };
    unsigned char* out = (unsigned char*)malloc(LEN);
    unsigned char* in = (unsigned char*)calloc(1, LEN);
    ASSERT_NOT_NULL(out);
    ASSERT_NOT_NULL(in);
    for (int i = 0; i < LEN; i++) out[i] = (unsigned char)(i * 131 + 7);

    int stalled[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, stalled));
    shrink_sender(stalled[0], stalled[1]);
    TcpSocket stuck = { stalled[0], 1 };
    errno = 0;
    int one = tcp_send_bytes_raw(&stuck, out, LEN);
    if (one < 0 && errno == EPERM) {
        printf("  SKIP tcp_send_all_delivers_through_short_sends: send denied by sandbox\n");
        close(stalled[0]); close(stalled[1]); free(out); free(in);
        return;
    }
    printf("  one send to a peer that is not reading: %d of %d bytes\n", one, LEN);
    ASSERT_TRUE(one < LEN);
    close(stalled[0]);
    close(stalled[1]);

    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    shrink_sender(fds[0], fds[1]);
    TcpSocket left = { fds[0], 1 };
    SlowReader r = { fds[1], in, LEN, 0 };
    pthread_t reader;
    ASSERT_EQ(0, pthread_create(&reader, NULL, slow_reader, &r));
    int all = tcp_send_all_raw(&left, out, LEN);
    shutdown(fds[0], SHUT_WR);   /* the reader's EOF, if not every byte went */
    pthread_join(reader, NULL);
    ASSERT_EQ(LEN, all);
    ASSERT_EQ(LEN, r.got);
    ASSERT_EQ(0, memcmp(out, in, LEN));

    /* Nothing to send is nothing sent, and no handle sends nothing. */
    ASSERT_EQ(0, tcp_send_all_raw(&left, out, 0));
    ASSERT_EQ(0, tcp_send_all_raw(NULL, out, 16));

    close(fds[0]);
    close(fds[1]);
    free(out);
    free(in);
}

#endif
