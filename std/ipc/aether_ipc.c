/* aether_ipc.c — std.ipc: a process's channel to its parent, and the socket
 * channel's reads and descriptor passing.
 *
 * Companion to std.os.run_pipe and std.os.run_socket (parent side).
 * Provides the function the child calls to discover the channel fd that
 * the parent opened, and the reads, writes and descriptor passing either
 * end uses on it.
 *
 * Mechanism: the parent sets AETHER_IPC_FD=<n> in the child's
 * environment before exec, and dup2's its end of the channel to
 * fd <n>. Child reads getenv("AETHER_IPC_FD"), parses to int,
 * verifies the fd is open. Returns fd or -1. os.run_socket also sets
 * AETHER_IPC_KIND=socket; a channel without it is a pipe.
 *
 * The env-var-as-primary design (rather than hardcoded fd 3)
 * sidesteps fd-allocation surprises in shell intermediaries
 * (e.g. `bash -c '<driver>'` between aeb and the test driver).
 *
 * Descriptor passing (send_fd / recv_fd) is SCM_RIGHTS on the socket
 * channel. The receiver is the side a hostile peer attacks, so recv_fd
 * accepts exactly one descriptor per message and closes every one that
 * arrived when it refuses a message: otherwise a peer could fill the
 * receiver's descriptor table with messages it rejects. */

#include "../../runtime/config/aether_optimization_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../string/aether_string.h"

typedef struct { const char* _0; const char* _1; } _tuple_string_string;
typedef struct { int _0; const char* _1; const char* _2; } _tuple_int_string_string;

#if AETHER_HAS_FILESYSTEM
/* The bytes of a string argument: an AetherString carries its length, so a
 * payload with a zero byte in it goes whole; a plain char* goes to its NUL. */
static const char* ipc_bytes(const void* obj, size_t* len) {
    if (!obj) {
        *len = 0;
        return "";
    }
    if (aether_string_is_string(obj)) {
        const AetherString* s = (const AetherString*)obj;
        *len = s->length;
        return s->data;
    }
    *len = strlen((const char*)obj);
    return (const char*)obj;
}
#endif

static _tuple_string_string ipc_read_failed(const char* err) {
    _tuple_string_string out = { (const char*)string_empty(), err };
    return out;
}

static _tuple_int_string_string ipc_recv_failed(const char* err) {
    _tuple_int_string_string out = { -1, (const char*)string_empty(), err };
    return out;
}

#if !AETHER_HAS_FILESYSTEM

int ipc_parent_channel_raw(void) { return -1; }
const char* ipc_parent_channel_kind_raw(void) { return ""; }
const char* ipc_write_raw(int fd, const void* bytes) {
    (void)fd; (void)bytes;
    return "ipc.write: unavailable in this build";
}
_tuple_string_string ipc_read_raw(int fd, int max) {
    (void)fd; (void)max;
    return ipc_read_failed("ipc.read: unavailable in this build");
}
const char* ipc_send_fd_raw(int ch, int fd, const void* payload) {
    (void)ch; (void)fd; (void)payload;
    return "ipc.send_fd: unavailable in this build";
}
_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_failed("ipc.recv_fd: unavailable in this build");
}

#elif !defined(_WIN32)

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

/* Sockets, and so the socket channel and descriptor passing, where there
 * are processes to talk between. WASI has a filesystem and descriptors but
 * neither: write and read stay plain descriptor I/O there. */
#if AETHER_HAS_PROCESS && !defined(__wasi__)
#define IPC_HAS_SOCKETS 1
#include <sys/socket.h>
#else
#define IPC_HAS_SOCKETS 0
#endif

/* Linux and FreeBSD refuse SIGPIPE per call; macOS has no MSG_NOSIGNAL and
 * carries it on the socket instead (SO_NOSIGPIPE, which os.socketpair and
 * os.run_socket set on both ends, and which a child's inherited end keeps). */
#if IPC_HAS_SOCKETS && defined(MSG_NOSIGNAL)
#define IPC_NOSIGNAL MSG_NOSIGNAL
#else
#define IPC_NOSIGNAL 0
#endif

/* The most descriptors one recvmsg makes room for. A message carrying more
 * is truncated by the kernel (MSG_CTRUNC), which closes what did not fit;
 * recv_fd closes the rest. Room for more than one is what lets recv_fd see
 * a second descriptor and refuse it, rather than only a truncation. */
#define IPC_MAX_FDS 8

/* "<what>: <strerror(err)>" in a per-thread buffer, valid until this
 * thread's next failing call. */
static const char* ipc_errno_msg(const char* what, int err) {
    static __thread char msg[256];
    char buf[128];
    const char* reason = buf;
    buf[0] = '\0';
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    reason = strerror_r(err, buf, sizeof buf);
#else
    if (strerror_r(err, buf, sizeof buf) != 0) snprintf(buf, sizeof buf, "error %d", err);
#endif
    snprintf(msg, sizeof msg, "%s: %s", what, reason);
    return msg;
}

int ipc_parent_channel_raw(void) {
    const char* s = getenv("AETHER_IPC_FD");
    if (!s || !*s) return -1;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    if (v < 0 || v > 65535) return -1;
    int fd = (int)v;
    /* Verify the fd is open. fcntl(F_GETFD) returns -1 with
     * errno=EBADF if not. */
    if (fcntl(fd, F_GETFD) < 0) return -1;
    return fd;
}

const char* ipc_parent_channel_kind_raw(void) {
    if (ipc_parent_channel_raw() < 0) return "";
    const char* k = getenv("AETHER_IPC_KIND");
    if (k && strcmp(k, "socket") == 0) return "socket";
    return "pipe";
}

/* Write all `len` bytes: short writes continue where they stopped and EINTR
 * retries, so success always means every byte went. A socket is written with
 * send(MSG_NOSIGNAL), so a gone peer is EPIPE rather than SIGPIPE; a pipe
 * (send says ENOTSOCK) with write(), as std.net's fd_write does. */
static const char* ipc_write_all(int fd, const char* data, size_t len, const char* what) {
    int is_socket = IPC_HAS_SOCKETS;
    size_t done = 0;
    while (done < len) {
#if IPC_HAS_SOCKETS
        ssize_t n = is_socket ? send(fd, data + done, len - done, IPC_NOSIGNAL)
                              : write(fd, data + done, len - done);
#else
        ssize_t n = write(fd, data + done, len - done);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            if (is_socket && errno == ENOTSOCK) {
                is_socket = 0;
                continue;
            }
            return ipc_errno_msg(what, errno);
        }
        if (n == 0) return ipc_errno_msg(what, EIO);
        done += (size_t)n;
    }
    return "";
}

const char* ipc_write_raw(int fd, const void* bytes) {
    if (fd < 0) return "ipc.write: invalid descriptor";
    size_t len = 0;
    const char* data = ipc_bytes(bytes, &len);
    return ipc_write_all(fd, data, len, "ipc.write");
}

/* One read of up to `max` bytes: what arrived, ("", "") at end of stream,
 * or ("", err). Binary-safe: the string carries its length. */
_tuple_string_string ipc_read_raw(int fd, int max) {
    if (fd < 0) return ipc_read_failed("ipc.read: invalid descriptor");
    if (max <= 0) return ipc_read_failed("ipc.read: max must be positive");
    char* buf = (char*)malloc((size_t)max);
    if (!buf) return ipc_read_failed("ipc.read: out of memory");
    ssize_t n;
    do { n = read(fd, buf, (size_t)max); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        int e = errno;
        free(buf);
        return ipc_read_failed(ipc_errno_msg("ipc.read", e));
    }
    AetherString* s = string_new_with_length(buf, (size_t)n);
    free(buf);
    if (!s) return ipc_read_failed("ipc.read: out of memory");
    _tuple_string_string out = { (const char*)s, "" };
    return out;
}

#if IPC_HAS_SOCKETS
/* Send `fd` with `payload` attached. The descriptor travels with the first
 * byte of the payload, so a sendmsg that sends only part of it has already
 * sent the descriptor, and the rest goes as plain bytes. */
const char* ipc_send_fd_raw(int ch, int fd, const void* payload) {
    if (ch < 0) return "ipc.send_fd: invalid channel";
    if (fd < 0) return "ipc.send_fd: invalid descriptor to send";
    size_t len = 0;
    const char* data = ipc_bytes(payload, &len);
    if (len == 0) {
        return "ipc.send_fd: empty payload (a descriptor travels attached to at least one byte)";
    }

    struct iovec iov;
    iov.iov_base = (void*)data;
    iov.iov_len = len;
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof control.buf;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    ssize_t n;
    do { n = sendmsg(ch, &msg, IPC_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n < 0) return ipc_errno_msg("ipc.send_fd", errno);
    if ((size_t)n < len) return ipc_write_all(ch, data + n, len - (size_t)n, "ipc.send_fd");
    return "";
}

static void ipc_close_all(const int* fds, size_t n) {
    for (size_t i = 0; i < n; i++) close(fds[i]);
}

/* Receive one descriptor and the bytes that came with it (up to `max`).
 *
 * The channel is a byte stream, so the bytes are whatever one recvmsg
 * returns: the payload sent with the descriptor, and possibly bytes written
 * after it. Framing is the caller's.
 *
 * Exactly one descriptor, in one SCM_RIGHTS message, or an error: on a
 * truncated control buffer, a control message of another kind, more than
 * one descriptor, or none, every descriptor that arrived is closed first.
 *
 * Close-on-exec at receipt: MSG_CMSG_CLOEXEC sets it as the descriptor is
 * made (Linux, FreeBSD). macOS lacks it and gets fcntl straight after
 * recvmsg, which leaves a window: a fork on another thread between the two
 * copies the descriptor into that child, and an exec there keeps it. That
 * is the race aether_pipe_cloexec closes with the fork lock; holding the
 * lock across a recvmsg that may block would stall every fork, so here it
 * stays open. */
_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    if (ch < 0) return ipc_recv_failed("ipc.recv_fd: invalid channel");
    if (max <= 0) return ipc_recv_failed("ipc.recv_fd: max must be positive");
    char* buf = (char*)malloc((size_t)max);
    if (!buf) return ipc_recv_failed("ipc.recv_fd: out of memory");

    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = (size_t)max;
    union {
        char buf[CMSG_SPACE(sizeof(int) * IPC_MAX_FDS)];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof control.buf;

    int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    ssize_t n;
    do { n = recvmsg(ch, &msg, flags); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        int e = errno;
        free(buf);
        return ipc_recv_failed(ipc_errno_msg("ipc.recv_fd", e));
    }

    /* Collect every descriptor that arrived before judging the message, so
     * a refusal can close them all. */
    int fds[IPC_MAX_FDS];
    size_t nfds = 0;
    int foreign = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
            foreign = 1;
            continue;
        }
        size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int fd;
            memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
            if (nfds < IPC_MAX_FDS) fds[nfds++] = fd;
            else close(fd);
        }
    }

    const char* err = NULL;
    if (msg.msg_flags & MSG_CTRUNC) {
        err = "ipc.recv_fd: control data truncated (too many descriptors in one message)";
    } else if (foreign) {
        err = "ipc.recv_fd: a control message other than SCM_RIGHTS";
    } else if (nfds > 1) {
        err = "ipc.recv_fd: more than one descriptor in one message";
    } else if (nfds == 0) {
        err = n == 0 ? "ipc.recv_fd: end of stream (peer closed)"
                     : "ipc.recv_fd: message carried no descriptor";
    }
    if (err) {
        ipc_close_all(fds, nfds);
        free(buf);
        return ipc_recv_failed(err);
    }

#ifndef MSG_CMSG_CLOEXEC
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
#endif
    AetherString* s = string_new_with_length(buf, (size_t)n);
    free(buf);
    if (!s) {
        close(fds[0]);
        return ipc_recv_failed("ipc.recv_fd: out of memory");
    }
    _tuple_int_string_string out = { fds[0], (const char*)s, "" };
    return out;
}
#else
const char* ipc_send_fd_raw(int ch, int fd, const void* payload) {
    (void)ch; (void)fd; (void)payload;
    return "ipc.send_fd: unavailable in this build (no sockets)";
}
_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_failed("ipc.recv_fd: unavailable in this build (no sockets)");
}
#endif /* IPC_HAS_SOCKETS */

#else /* _WIN32 */

#include <io.h>

int ipc_parent_channel_raw(void) {
    /* Windows handle inheritance for fd-mapped pipes is
     * non-trivial (STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST
     * + _open_osfhandle on both sides). Even if AETHER_IPC_FD is
     * set, there's no portable way to verify the descriptor is
     * open as a pipe via the Win32 CRT layer. v1 ships POSIX-only;
     * Windows consumers fall back to file-marker patterns. */
    return -1;
}

const char* ipc_parent_channel_kind_raw(void) { return ""; }

/* write and read stay plain CRT descriptor I/O here, as std.net's fd_write
 * is; the socket channel and descriptor passing are POSIX-only (Windows
 * passes handles with DuplicateHandle, a different shape). */
const char* ipc_write_raw(int fd, const void* bytes) {
    if (fd < 0) return "ipc.write: invalid descriptor";
    size_t len = 0;
    const char* data = ipc_bytes(bytes, &len);
    size_t done = 0;
    while (done < len) {
        unsigned chunk = (len - done) > 0x7fffffffu ? 0x7fffffffu : (unsigned)(len - done);
        int n = _write(fd, data + done, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            return "ipc.write: write failed";
        }
        if (n == 0) return "ipc.write: write failed";
        done += (size_t)n;
    }
    return "";
}

_tuple_string_string ipc_read_raw(int fd, int max) {
    if (fd < 0) return ipc_read_failed("ipc.read: invalid descriptor");
    if (max <= 0) return ipc_read_failed("ipc.read: max must be positive");
    char* buf = (char*)malloc((size_t)max);
    if (!buf) return ipc_read_failed("ipc.read: out of memory");
    int n = _read(fd, buf, (unsigned)max);
    if (n < 0) {
        free(buf);
        return ipc_read_failed("ipc.read: read failed");
    }
    AetherString* s = string_new_with_length(buf, (size_t)n);
    free(buf);
    if (!s) return ipc_read_failed("ipc.read: out of memory");
    _tuple_string_string out = { (const char*)s, "" };
    return out;
}

const char* ipc_send_fd_raw(int ch, int fd, const void* payload) {
    (void)ch; (void)fd; (void)payload;
    return "ipc.send_fd: unsupported on Windows";
}

_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_failed("ipc.recv_fd: unsupported on Windows");
}

#endif /* AETHER_HAS_FILESYSTEM / _WIN32 */
