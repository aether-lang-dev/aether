#ifndef AETHER_UDP_H
#define AETHER_UDP_H

/* std.udp — datagram sockets for game networking (#2201).
 *
 * Every socket is NON-BLOCKING from the moment it is bound: a game loop
 * drains the socket once a tick and must never wait on it. recv on an
 * empty socket reports "would block" (the `UDP_RECV_WOULDBLOCK` code)
 * rather than stalling; `udp_poll_raw` is the way to wait when a program
 * does want to.
 *
 * A UdpAddr is a resolved peer: a value that can be compared, hashed,
 * printed and sent to again without touching the resolver, so a server
 * answering 64 peers a tick resolves each of them once. */

typedef struct UdpSocket UdpSocket;
typedef struct UdpAddr UdpAddr;

/* Tuple layout for `udp_recv_from_raw`: (bytes, sender, err). */
typedef struct UdpRecvResult {
    int _0;          /* bytes received, 0 when there is no packet */
    void* _1;        /* UdpAddr* of the sender, NULL when there is no packet */
    const char* _2;  /* "" on a packet, "would block" when idle, else the error */
} UdpRecvResult;

/* Return codes of `udp_recv_from_into_raw` when no packet was read. */
#define UDP_RECV_WOULDBLOCK (-1)
#define UDP_RECV_ERROR      (-2)

/* Bind a datagram socket on `host` ("0.0.0.0" or "" for every IPv4
 * interface, "::" for IPv6 — dual-stack where the OS allows it —
 * "127.0.0.1" / "::1" for loopback only). Port 0 asks the OS for an
 * ephemeral port, readable through `udp_local_port_raw`. The socket is
 * non-blocking. NULL on failure. */
UdpSocket* udp_bind_raw(const char* host, int port);
int udp_local_port_raw(UdpSocket* sock);
int udp_close(UdpSocket* sock);

/* OS descriptor inside the handle, or -1 for a null handle. Owned by the
 * handle: for polling several sockets at once, or capsicum narrowing,
 * never for close(). */
int udp_fd_raw(UdpSocket* sock);

/* 1 = a datagram is waiting (recv will not block), 0 = timeout, -1 =
 * null handle or poll error. timeout_ms: -1 blocks, 0 polls. */
int udp_poll_raw(UdpSocket* sock, int timeout_ms);

/* SO_SNDBUF / SO_RCVBUF in bytes; 0 leaves that side alone. 0 on success,
 * -1 on a null handle or setsockopt failure. */
int udp_set_buffer_sizes_raw(UdpSocket* sock, int send_bytes, int recv_bytes);
/* SO_BROADCAST, for LAN discovery on an IPv4 socket. 0 / -1 as above. */
int udp_set_broadcast_raw(UdpSocket* sock, int on);

/* Resolve host:port to a peer address once. NULL when the name does not
 * resolve. The result is owned by the caller: `udp_addr_free`. */
UdpAddr* udp_resolve_raw(const char* host, int port);
/* An unset address for `udp_recv_from_into_raw` to fill. */
UdpAddr* udp_addr_new(void);
UdpAddr* udp_addr_clone_raw(const UdpAddr* addr);
int udp_addr_free(UdpAddr* addr);

/* Value semantics over a resolved address. `host` is the numeric form
 * ("10.0.0.7", "fe80::1") as a fresh AetherString*; `port` is -1 and
 * `host` NULL for a null or unset address. `equal` is 1 when family, address
 * and port all match; `hash` is stable for equal addresses within a
 * process and non-negative. */
void* udp_addr_host_raw(const UdpAddr* addr);
int udp_addr_port_raw(const UdpAddr* addr);
int udp_addr_equal_raw(const UdpAddr* a, const UdpAddr* b);
int udp_addr_hash_raw(const UdpAddr* addr);

/* Send one datagram of `n` bytes. Returns the byte count sent, or -1 on a
 * null handle, an unresolvable host, or a send failure (a full send buffer
 * on a non-blocking socket reports -1 too: the packet is dropped, which is
 * what a datagram protocol expects). The `_addr` form skips resolution. */
int udp_send_to_raw(UdpSocket* sock, const char* host, int port, const void* data, int n);
int udp_send_to_addr_raw(UdpSocket* sock, const UdpAddr* addr, const void* data, int n);

/* Receive one datagram into `buf` (at most `cap` bytes; a longer datagram
 * is truncated to `cap` and the rest discarded, per recvfrom(2)). Never
 * blocks.
 *
 * `_into`: fills `addr` (may be NULL when the sender is not wanted) and
 * returns the byte count, UDP_RECV_WOULDBLOCK when nothing is waiting, or
 * UDP_RECV_ERROR on a null handle, a bad buffer, or a socket error.
 *
 * The tuple form allocates the sender address for the caller, who frees
 * it with `udp_addr_free`; the sender is NULL and the count 0 whenever
 * `_2` is not "". */
int udp_recv_from_into_raw(UdpSocket* sock, void* buf, int cap, UdpAddr* addr);
UdpRecvResult udp_recv_from_raw(UdpSocket* sock, void* buf, int cap);

#endif
