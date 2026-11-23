#ifndef AETHER_NET_H
#define AETHER_NET_H

#include <stddef.h>

typedef struct TcpSocket TcpSocket;
typedef struct TcpServer TcpServer;
// A cancel handle (tcp_cancel_new_raw), bound to sockets by
// tcp_set_deadline_raw or tcp_connect_until_raw.
typedef struct TcpCancel TcpCancel;
typedef struct TcpReceiveResult {
    void* _0;
    int _1;
    const char* _2;
} TcpReceiveResult;

// The (handle, error) pair the Unix-socket entry points return: the handle,
// or NULL with a static message saying why.
typedef struct TcpOpenResult {
    void* _0;
    const char* _1;
} TcpOpenResult;

// TCP Client
TcpSocket* tcp_connect_raw(const char* host, int port);
// tcp_connect_raw with the receive buffer (SO_RCVBUF) set to `read_buffer`
// bytes before the connection is made; NULL when `read_buffer` is not positive.
TcpSocket* tcp_connect_buffered_raw(const char* host, int port, int read_buffer);
// tcp_connect_raw held to `timeout_ms` for the whole dial (every address the
// host resolves to): the socket, or NULL with why ("connect timeout" when the
// budget ran out, "could not resolve host", "connection failed", the sandbox's
// refusal). `timeout_ms` must be positive.
TcpOpenResult tcp_connect_timeout_raw(const char* host, int port, int timeout_ms);
int tcp_send_raw(TcpSocket* sock, const char* data);
int tcp_send_n_raw(TcpSocket* sock, const char* data, int length);
// The bytes received as a fresh counted AetherString (cast to char*) the
// caller releases; NULL on error or close.
char* tcp_receive_raw(TcpSocket* sock, int max_bytes);
TcpReceiveResult tcp_receive_n_raw(TcpSocket* sock, int max_bytes);
// Byte-buffer forms for std.tcp's slice wrappers (#2301). The Aether
// wrapper checks the slice's bound and passes its length; these trust it.
// tcp_send_bytes_raw: send(2) of `length` bytes from `data`; the count
//   (possibly short) or -1.
// tcp_receive_into_raw: one recv(2) of at most `cap` bytes into `buf`;
//   the count (> 0), TCP_RECV_TIMEOUT for an idle peer (still connected),
//   or TCP_RECV_CLOSED for a FIN, an error, a closed handle or a bad buffer.
#define TCP_RECV_CLOSED  (-1)
#define TCP_RECV_TIMEOUT (-2)
// A receive on a socket with a deadline or a cancel bound to it
// (tcp_set_deadline_raw) also ends with these; the socket stays connected,
// and its owner closes it.
#define TCP_RECV_DEADLINE  (-3)
#define TCP_RECV_CANCELLED (-4)
// tcp_stop_raw: what stopped the last operation a bound deadline or cancel
// stopped.
#define TCP_STOP_NONE      0
#define TCP_STOP_DEADLINE  1
#define TCP_STOP_CANCELLED 2
// The errors for those two stops, the same text std.tcp's DEADLINE_EXCEEDED
// and CANCELLED hold.
#define TCP_ERR_DEADLINE  "deadline exceeded"
#define TCP_ERR_CANCELLED "cancelled"
int tcp_send_bytes_raw(TcpSocket* sock, const void* data, int length);
// tcp_send_all_raw: every one of `length` bytes from `data`, one send(2)
//   after another (#3108); the bytes sent, `length` unless a send failed.
int tcp_send_all_raw(TcpSocket* sock, const void* data, int length);
int tcp_receive_into_raw(TcpSocket* sock, void* buf, int cap);
// tcp_reset_raw: 1 when the last receive on `sock` (tcp_receive_raw,
//   tcp_receive_n_raw, tcp_receive_into_raw) failed because the peer reset
//   or aborted the connection, else 0, a null handle included. Read from
//   the error that receive left, when it failed, and kept on the socket, so
//   a layer over the socket (the pure-Aether TLS client) can tell a reset
//   from its own failures without an errno something later may have set.
int tcp_reset_raw(TcpSocket* sock);
int tcp_close(TcpSocket* sock);

// Unix-domain stream sockets, as the same TcpSocket / TcpServer handles, so
// every read, write, poll and close above works on them unchanged.
// tcp_connect_unix_raw: connect to the socket file at `path`; in an enforce
//   block it needs fs_write on `path` (connecting to a Unix socket needs
//   write permission on its file, the kernel's rule).
// tcp_listen_unix_raw: bind a socket file at `path` and listen; needs
//   tcp_listen and fs_write on `path`. The file must not exist yet;
//   tcp_server_close removes it.
// A path longer than sockaddr_un holds (103 bytes on macOS and the BSDs,
// 107 on Linux and Windows) is refused, never truncated.
TcpOpenResult tcp_connect_unix_raw(const char* path);
TcpOpenResult tcp_listen_unix_raw(const char* path);

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
// SO_RCVBUF on a connected socket, which also stops the kernel growing it:
// 0 on success, -1 on a null/closed handle, bytes <= 0, or setsockopt failure.
int tcp_set_read_buffer_raw(TcpSocket* sock, int bytes);
// How many received bytes wait to be read (FIONREAD), or -1 on error.
int tcp_available_raw(TcpSocket* sock);
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
// The same, keeping the receive and send timeouts the owner set on the fd
// rather than putting std.tcp's 30 s ones on it (#3703).
TcpSocket* tcp_socket_adopt_fd(int fd);

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

// The write side of the same: a socket write that cannot wait on a peer
// that has stopped reading.
// tcp_wait_writable_raw: 1 = room to send (or an error or hangup the send
//                will report), 0 = timeout, -1 = null/closed handle or poll
//                error. timeout_ms as tcp_poll_raw's.
// tcp_send_some_raw: what the socket takes now of `length` bytes from
//                `data`, never waiting for room: the count (0 when the send
//                buffer is full), or -1 on an error (the connection gone).
int tcp_wait_writable_raw(TcpSocket* sock, int timeout_ms);
int tcp_send_some_raw(TcpSocket* sock, const void* data, int length);

// Deadlines and cancellation, per connection (std.tcp's connect_until,
// set_deadline, cancel_new; docs/design/tcp-tls-deadlines-and-cancellation.md).
// due_ms is an absolute time on the monotonic clock std.os's
// now_monotonic_ms reads, in ms; 0 means none.
// tcp_connect_until_raw: a dial bounded by both, the socket coming back
//                with both bound; "deadline exceeded" or "cancelled".
// tcp_set_deadline_raw: bind them to a socket (replacing what was bound,
//                clearing its stop); 0, or -1 for a null handle or a
//                negative due_ms.
// tcp_deadline_raw / tcp_stop_raw: what is bound, and what stopped the
//                last operation (TCP_STOP_*).
// tcp_cancel_new_raw: a fresh handle, or NULL with why.
// tcp_cancel_raw: trip it, from any thread (on POSIX, a signal handler
//                too); idempotent. It never touches a socket's descriptor.
// tcp_cancel_tripped_raw: 1 once tripped.
// tcp_cancel_retain_raw: another reference, for each other thread that may
//                trip the handle; 0, or -1 for a null handle.
// tcp_cancel_free_raw: drop one reference (the creator's or a retained
//                one); a bound socket keeps the handle alive until it is
//                closed, and the last drop closes and frees it.
TcpOpenResult tcp_connect_until_raw(const char* host, int port, long long due_ms, TcpCancel* cancel);
int tcp_set_deadline_raw(TcpSocket* sock, long long due_ms, TcpCancel* cancel);
long long tcp_deadline_raw(TcpSocket* sock);
int tcp_stop_raw(TcpSocket* sock);
TcpOpenResult tcp_cancel_new_raw(void);
int tcp_cancel_raw(TcpCancel* cancel);
int tcp_cancel_tripped_raw(TcpCancel* cancel);
int tcp_cancel_retain_raw(TcpCancel* cancel);
void tcp_cancel_free_raw(TcpCancel* cancel);

// Shared by std.udp (#2201): one-time platform socket init (WSAStartup on
// Windows, a no-op elsewhere), and the "was that EAGAIN/EWOULDBLOCK/EINTR"
// test both modules apply after a recv/send that returned < 0.
void aether_net_init(void);
int aether_net_wouldblock(void);

#endif
