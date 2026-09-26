#ifndef AETHER_NET_H
#define AETHER_NET_H

#include <stddef.h>

typedef struct TcpSocket TcpSocket;
typedef struct TcpServer TcpServer;
typedef struct TcpReceiveResult {
    void* _0;
    int _1;
    const char* _2;
} TcpReceiveResult;

// TCP Client
TcpSocket* tcp_connect_raw(const char* host, int port);
int tcp_send_raw(TcpSocket* sock, const char* data);
int tcp_send_n_raw(TcpSocket* sock, const char* data, int length);
char* tcp_receive_raw(TcpSocket* sock, int max_bytes);
TcpReceiveResult tcp_receive_n_raw(TcpSocket* sock, int max_bytes);
int tcp_close(TcpSocket* sock);

// TCP Server
TcpServer* tcp_listen_raw(int port);
// #2136: listen on ONE address ("127.0.0.1" for a loopback-only debugging
// channel, "0.0.0.0" for every interface); port 0 asks the OS for an
// ephemeral port, readable afterwards through tcp_server_port_raw.
TcpServer* tcp_listen_on_raw(const char* address, int port);
// The port a server is bound to (the ephemeral one for a listen on 0),
// or -1 for a null handle.
int tcp_server_port_raw(TcpServer* server);
// Wait up to timeout_ms for a connection to be waiting in accept's queue:
// 1 = accept will not block, 0 = timeout, -1 = null handle / error. A
// server loop with a stop flag polls this instead of blocking in accept.
int tcp_server_poll_raw(TcpServer* server, int timeout_ms);
// TCP_NODELAY on a connected socket: 0 on success, -1 on a null/closed
// handle or setsockopt failure.
int tcp_set_nodelay_raw(TcpSocket* sock, int on);
TcpSocket* tcp_accept_raw(TcpServer* server);
int tcp_server_close(TcpServer* server);

// OS-level descriptor inside a handle, or -1 if null/closed. For
// capability plumbing (capsicum.rights_limit before capsicum.enter);
// owned by the handle — do not close() it. Issue #1003.
int tcp_fd_raw(TcpSocket* sock);
int tcp_server_fd_raw(TcpServer* server);

// Adopt an already-connected OS fd into a std.tcp socket handle.
// Ownership transfers to the returned TcpSocket; tcp_close closes it.
TcpSocket* tcp_socket_from_fd_owned(int fd);

// Readiness primitives for full-duplex relaying (#1092). Wrap poll(2);
// neither reads nor mutates the socket's connected flag.
//
// tcp_poll_raw:  1 = readable (data or EOF pending), 0 = timeout,
//                -1 = null/closed handle or poll error. timeout_ms:
//                -1 blocks, 0 polls, >0 waits that many milliseconds.
// tcp_poll2_raw: wait on two sockets at once; returns a bitmask
//                (1 = a readable, 2 = b readable, 3 = both), 0 on
//                timeout, -1 if both are null/closed or on poll error.
int tcp_poll_raw(TcpSocket* sock, int timeout_ms);
int tcp_poll2_raw(TcpSocket* a, TcpSocket* b, int timeout_ms);

// Shared by std.udp (#2201): one-time platform socket init (WSAStartup on
// Windows, a no-op elsewhere), and the "was that EAGAIN/EWOULDBLOCK/EINTR"
// test both modules apply after a recv/send that returned < 0.
void aether_net_init(void);
int aether_net_wouldblock(void);

#endif
