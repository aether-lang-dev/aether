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
 * receiver's descriptor table with messages it rejects.
 *
 * recv is the one receive: the bytes and the descriptor, if any, from one
 * recvmsg. The parent end of a channel os.run_socket made (marked in
 * aether_spawn.c's table) refuses any descriptor, in recv and recv_fd alike:
 * a child has no business handing its parent descriptors.
 *
 * Frames (write_frame / read_frame) are a 4-byte big-endian length and
 * that many bytes, the reader capped by the caller's max. wait_readable and
 * the timeout form of read_frame bound a wait with poll(); on Windows, where
 * a CRT descriptor has no poll(), they ask an anonymous pipe how many bytes
 * wait (PeekNamedPipe), sleeping briefly between asks.
 *
 * os.run_stdio's channel is the child's own stdin and stdout (its env
 * carries AETHER_IPC_KIND=stdio, and no AETHER_IPC_FD). */

#include "../../runtime/config/aether_optimization_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../string/aether_string.h"
#include "../../runtime/utils/aether_spawn.h"

typedef struct { const char* _0; const char* _1; } _tuple_string_string;
typedef struct { int _0; const char* _1; const char* _2; } _tuple_int_string_string;
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;
typedef struct { int _0; const char* _1; } _tuple_int_string;
#if AETHER_HAS_FILESYSTEM
/* AETHER_IPC_KIND=stdio: os.run_stdio started this process. */
static int ipc_env_says_stdio(void) {
    const char* k = getenv("AETHER_IPC_KIND");
    return k && strcmp(k, "stdio") == 0;
}
#endif

#if AETHER_HAS_FILESYSTEM
#include <stdint.h>
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

static _tuple_string_int_string ipc_recv_msg_failed(const char* err) {
    _tuple_string_int_string out = { (const char*)string_empty(), -1, err };
    return out;
}

static _tuple_int_string ipc_wait_failed(const char* err) {
    _tuple_int_string out = { 0, err };
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
_tuple_string_int_string ipc_recv_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_msg_failed("ipc.recv: unavailable in this build");
}
const char* ipc_write_frame_raw(int ch, const void* data) {
    (void)ch; (void)data;
    return "ipc.write_frame: unavailable in this build";
}
_tuple_string_string ipc_read_frame_raw(int ch, int max, int timeout_ms) {
    (void)ch; (void)max; (void)timeout_ms;
    return ipc_read_failed("ipc.read_frame: unavailable in this build");
}
_tuple_int_string ipc_wait_readable_raw(int ch, int timeout_ms) {
    (void)ch; (void)timeout_ms;
    return ipc_wait_failed("ipc.wait_readable: unavailable in this build");
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
#include <pthread.h>
#include <signal.h>
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

/* The most descriptors one recvmsg makes room for: more than any one
 * message can carry (Linux's SCM_MAX_FD is 253, macOS refuses past 254), so
 * every descriptor a peer sends is seen and a refusal closes them all. Room
 * for fewer is not safe: macOS truncates the control data (MSG_CTRUNC) but
 * still installs every descriptor, and those past the buffer stay open in
 * this process with no way to name them. A truncation (another kernel's
 * limit) is still refused. */
#define IPC_MAX_FDS 256

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
    if (ipc_env_says_stdio()) return "stdio";
    if (ipc_parent_channel_raw() < 0) return "";
    const char* k = getenv("AETHER_IPC_KIND");
    if (k && strcmp(k, "socket") == 0) return "socket";
    return "pipe";
}

#if IPC_HAS_SOCKETS
/* write(2) to a pipe with SIGPIPE held off this thread: a reader that has
 * gone is EPIPE, not a signal that ends the process (a pipe has no
 * MSG_NOSIGNAL or SO_NOSIGPIPE). SIGPIPE is blocked for the call; when the
 * write raised one that was not already pending, it is taken off again with
 * sigwait (it is pending, so that returns at once) before the old mask comes
 * back. A SIGPIPE pending before the call is left for its owner. */
static ssize_t ipc_write_nosigpipe(int fd, const void* buf, size_t len) {
    sigset_t pipe_only, old, pending;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    sigemptyset(&pending);
    sigpending(&pending);
    int was_pending = sigismember(&pending, SIGPIPE) == 1;
    if (pthread_sigmask(SIG_BLOCK, &pipe_only, &old) != 0) return write(fd, buf, len);
    ssize_t n = write(fd, buf, len);
    int e = errno;
    if (n < 0 && e == EPIPE && !was_pending) {
        sigemptyset(&pending);
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
            int sig = 0;
            sigwait(&pipe_only, &sig);
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = e;
    return n;
}
#endif

/* Write all `len` bytes: short writes continue where they stopped and EINTR
 * retries, so success always means every byte went. A socket is written with
 * send(MSG_NOSIGNAL), so a gone peer is EPIPE rather than SIGPIPE; a pipe
 * (send says ENOTSOCK) with write() under ipc_write_nosigpipe, so a gone
 * reader is EPIPE there too. */
static const char* ipc_write_all(int fd, const char* data, size_t len, const char* what) {
    int is_socket = IPC_HAS_SOCKETS;
    size_t done = 0;
    while (done < len) {
#if IPC_HAS_SOCKETS
        ssize_t n = is_socket ? send(fd, data + done, len - done, IPC_NOSIGNAL)
                              : ipc_write_nosigpipe(fd, data + done, len - done);
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

/* "<what>: <text>" in a per-thread buffer of its own, valid until this
 * thread's next call. */
static const char* ipc_msg(const char* what, const char* text) {
    static __thread char msg[256];
    snprintf(msg, sizeof msg, "%s: %s", what, text);
    return msg;
}

/* One recvmsg for recv and recv_fd: up to `max` bytes in *out (malloc'd, the
 * caller's) and the descriptor that came with them in *out_fd, -1 when none.
 *
 * The channel is a byte stream, so the bytes are whatever one recvmsg
 * returns: the payload sent with the descriptor, and possibly bytes written
 * after it. Framing is the caller's.
 *
 * At most one descriptor, in one SCM_RIGHTS message, or an error: on a
 * truncated control buffer, a control message of another kind, more than
 * one descriptor, or any at all on the parent end of a channel, every
 * descriptor that arrived is closed first, and the bytes with them are
 * dropped. `want_fd` (recv_fd) makes a message without a descriptor an
 * error too, and end of stream.
 *
 * Close-on-exec at receipt: MSG_CMSG_CLOEXEC sets it as the descriptor is
 * made (Linux, FreeBSD). macOS lacks it and gets fcntl straight after
 * recvmsg, which leaves a window: a fork on another thread between the two
 * copies the descriptor into that child, and an exec there keeps it. That
 * is the race aether_pipe_cloexec closes with the fork lock; holding the
 * lock across a recvmsg that may block would stall every fork, so here it
 * stays open.
 *
 * Returns NULL, or the error (*out and *out_fd are then unset). */
static const char* ipc_recv_core(int ch, int max, const char* what, int want_fd,
                                 char** out, size_t* out_n, int* out_fd) {
    if (ch < 0) return ipc_msg(what, "invalid channel");
    if (max <= 0) return ipc_msg(what, "max must be positive");
    char* buf = (char*)malloc((size_t)max);
    if (!buf) return ipc_msg(what, "out of memory");

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
        return ipc_errno_msg(what, e);
    }

    /* Collect every descriptor that arrived before judging the message, so
     * a refusal can close them all. */
    int fds[IPC_MAX_FDS];
    size_t nfds = 0;
    int foreign = 0;
    /* Only the bytes the kernel filled count. Under MSG_CTRUNC macOS leaves
     * cmsg_len at what was sent (ten descriptors' worth in a buffer with
     * room for eight); the descriptors it claims past the buffer are stack,
     * and closing them would close whatever descriptors they happened to
     * name. */
    const char* control_end = (const char*)msg.msg_control + msg.msg_controllen;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if ((const char*)c + CMSG_LEN(0) > control_end) break;
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
            foreign = 1;
            continue;
        }
        size_t len = c->cmsg_len;
        size_t room = (size_t)(control_end - (const char*)c);
        if (len > room) len = room;
        if (len < CMSG_LEN(0)) break;
        size_t count = (len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) {
            int fd;
            memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
            if (nfds < IPC_MAX_FDS) fds[nfds++] = fd;
            else close(fd);
        }
    }

    const char* err = NULL;
    if (msg.msg_flags & MSG_CTRUNC) {
        err = ipc_msg(what, "control data truncated (too many descriptors in one message)");
    } else if (foreign) {
        err = ipc_msg(what, "a control message other than SCM_RIGHTS");
    } else if (nfds > 0 && aether_ipc_is_parent_end(ch)) {
        err = ipc_msg(what, "descriptor from the child refused (the parent end of a channel takes none)");
    } else if (nfds > 1) {
        err = ipc_msg(what, "more than one descriptor in one message");
    } else if (nfds == 0 && want_fd) {
        err = ipc_msg(what, n == 0 ? "end of stream (peer closed)"
                                   : "message carried no descriptor");
    }
    if (err) {
        ipc_close_all(fds, nfds);
        free(buf);
        return err;
    }

#ifndef MSG_CMSG_CLOEXEC
    if (nfds == 1) fcntl(fds[0], F_SETFD, FD_CLOEXEC);
#endif
    *out = buf;
    *out_n = (size_t)n;
    *out_fd = nfds == 1 ? fds[0] : -1;
    return NULL;
}

/* Receive one descriptor and the bytes that came with it (up to `max`). */
_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    char* buf = NULL;
    size_t n = 0;
    int fd = -1;
    const char* err = ipc_recv_core(ch, max, "ipc.recv_fd", 1, &buf, &n, &fd);
    if (err) return ipc_recv_failed(err);
    AetherString* s = string_new_with_length(buf, n);
    free(buf);
    if (!s) {
        close(fd);
        return ipc_recv_failed("ipc.recv_fd: out of memory");
    }
    _tuple_int_string_string out = { fd, (const char*)s, "" };
    return out;
}

/* One receive: the bytes (up to `max`) and the descriptor, if one came with
 * them. ("", -1, "") at end of stream. */
_tuple_string_int_string ipc_recv_raw(int ch, int max) {
    char* buf = NULL;
    size_t n = 0;
    int fd = -1;
    const char* err = ipc_recv_core(ch, max, "ipc.recv", 0, &buf, &n, &fd);
    if (err) return ipc_recv_msg_failed(err);
    AetherString* s = string_new_with_length(buf, n);
    free(buf);
    if (!s) {
        if (fd >= 0) close(fd);
        return ipc_recv_msg_failed("ipc.recv: out of memory");
    }
    _tuple_string_int_string out = { (const char*)s, fd, "" };
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
_tuple_string_int_string ipc_recv_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_msg_failed("ipc.recv: unavailable in this build (no sockets)");
}
#endif /* IPC_HAS_SOCKETS */

/* The primitives the frame code below is written over (it is written once,
 * for every platform). now_ms is a monotonic clock; fill reads exactly `len`
 * bytes into dst, giving up at the absolute time `deadline_ms` (none when
 * negative) without ever reading past `len`. */
#include <poll.h>
#include <time.h>

static long long ipc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#if IPC_HAS_SOCKETS
/* poll(2) for readability for up to `ms` (negative: forever). 1 readable (or
 * hung up, or errored: a read then will not block), 0 timed out, -1 failed
 * with errno set. */
static int ipc_poll_in(int fd, long long ms) {
    long long end = ms < 0 ? -1 : ipc_now_ms() + ms;
    for (;;) {
        int wait = -1;
        if (end >= 0) {
            long long left = end - ipc_now_ms();
            wait = left <= 0 ? 0 : (left > 0x7fffffff ? 0x7fffffff : (int)left);
        }
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        int r = poll(&p, 1, wait);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            if (end >= 0 && ipc_now_ms() < end) continue;   /* woke early */
            return 0;
        }
        if (p.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        return 1;
    }
}
#endif

#define IPC_FILL_OK 0
#define IPC_FILL_EOF 1
#define IPC_FILL_TIMEOUT 2
#define IPC_FILL_ERR 3

static int ipc_fill(int fd, char* dst, size_t len, long long deadline_ms, size_t* got, int* err) {
    *got = 0;
    while (*got < len) {
        if (deadline_ms >= 0) {
#if IPC_HAS_SOCKETS
            long long left = deadline_ms - ipc_now_ms();
            int r = ipc_poll_in(fd, left < 0 ? 0 : left);
            if (r < 0) { *err = errno; return IPC_FILL_ERR; }
            if (r == 0) return IPC_FILL_TIMEOUT;
#else
            *err = ENOSYS;
            return IPC_FILL_ERR;
#endif
        }
        ssize_t n = read(fd, dst + *got, len - *got);
        if (n < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            return IPC_FILL_ERR;
        }
        if (n == 0) return IPC_FILL_EOF;
        *got += (size_t)n;
    }
    return IPC_FILL_OK;
}

_tuple_int_string ipc_wait_readable_raw(int ch, int timeout_ms) {
    if (ch < 0) return ipc_wait_failed("ipc.wait_readable: invalid descriptor");
#if IPC_HAS_SOCKETS
    int r = ipc_poll_in(ch, timeout_ms);
    if (r < 0) return ipc_wait_failed(ipc_errno_msg("ipc.wait_readable", errno));
    _tuple_int_string out = { r, "" };
    return out;
#else
    (void)timeout_ms;
    return ipc_wait_failed("ipc.wait_readable: unavailable in this build (no sockets)");
#endif
}

#else /* _WIN32 */

#include <fcntl.h>
#include <io.h>
#include <windows.h>

int ipc_parent_channel_raw(void) {
    /* Windows handle inheritance for fd-mapped pipes is
     * non-trivial (STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST
     * + _open_osfhandle on both sides). Even if AETHER_IPC_FD is
     * set, there's no portable way to verify the descriptor is
     * open as a pipe via the Win32 CRT layer. v1 ships POSIX-only;
     * Windows consumers fall back to file-marker patterns, or the stdio
     * channel (os.run_stdio). */
    return -1;
}

const char* ipc_parent_channel_kind_raw(void) { return ipc_env_says_stdio() ? "stdio" : ""; }

/* write and read stay plain CRT descriptor I/O here, as std.net's fd_write
 * is; the socket channel and descriptor passing are POSIX-only (Windows
 * passes handles with DuplicateHandle, a different shape). */
/* "<what>: <strerror(err)>"; one buffer, as the rest of the Windows path is
 * not called from several threads at once. */
static const char* ipc_errno_msg(const char* what, int err) {
    static char msg[256];
    snprintf(msg, sizeof msg, "%s: %s", what, strerror(err));
    return msg;
}

/* A CRT descriptor starts in text mode, a child's fd 0 and fd 1 included:
 * a read turns CR LF into LF and stops at a 0x1a byte, a write turns LF into
 * CR LF. Bytes on a channel are bytes, so every call here puts the
 * descriptor in binary mode before it uses it. */
static void ipc_binary(int fd) {
    if (fd >= 0) _setmode(fd, _O_BINARY);
}

static const char* ipc_write_all(int fd, const char* data, size_t len, const char* what) {
    ipc_binary(fd);
    size_t done = 0;
    while (done < len) {
        unsigned chunk = (len - done) > 0x7fffffffu ? 0x7fffffffu : (unsigned)(len - done);
        int n = _write(fd, data + done, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            return ipc_errno_msg(what, errno ? errno : EIO);
        }
        if (n == 0) return ipc_errno_msg(what, errno ? errno : EIO);
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

_tuple_string_string ipc_read_raw(int fd, int max) {
    if (fd < 0) return ipc_read_failed("ipc.read: invalid descriptor");
    if (max <= 0) return ipc_read_failed("ipc.read: max must be positive");
    ipc_binary(fd);
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

_tuple_string_int_string ipc_recv_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_msg_failed("ipc.recv: unsupported on Windows");
}

/* The frame code below over CRT descriptors. A CRT descriptor has no
 * poll(); a deadline is kept on a pipe by asking it how many bytes wait. */
static long long ipc_now_ms(void) { return (long long)GetTickCount64(); }

/* ipc_pipe_wait's answers past 1 (ready) and 0 (timed out). */
#define IPC_WAIT_BAD_FD (-1)
#define IPC_WAIT_NOT_PIPE (-2)
#define IPC_WAIT_FAILED (-3)

#define IPC_NOT_A_PIPE "not a pipe (a wait on Windows needs a pipe, such as os.run_stdio's)"

/* Wait up to `ms` (negative: forever; 0: ask once) for a byte to read on
 * the pipe under CRT descriptor `fd`. 1 when one waits, or when the writer
 * has closed (the read then gives end of stream), 0 at the deadline, else
 * one of IPC_WAIT_*. *avail_out, when given, is how many bytes wait (0 on a
 * closed pipe). PeekNamedPipe does not block, so this sleeps between asks,
 * 1 ms doubling to 8: Sleep rounds up to the system timer tick (about
 * 15.6 ms unless something raised the timer resolution), so a byte or the
 * deadline is noticed within about a tick. */
static int ipc_pipe_wait(int fd, long long ms, DWORD* avail_out) {
    if (avail_out) *avail_out = 0;
    intptr_t osh = _get_osfhandle(fd);
    if (osh == -1 || osh == -2) return IPC_WAIT_BAD_FD;
    HANDLE h = (HANDLE)osh;
    if (GetFileType(h) != FILE_TYPE_PIPE) return IPC_WAIT_NOT_PIPE;
    long long end = ms < 0 ? -1 : ipc_now_ms() + ms;
    DWORD nap = 1;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            DWORD le = GetLastError();
            if (le == ERROR_BROKEN_PIPE || le == ERROR_PIPE_NOT_CONNECTED || le == ERROR_NO_DATA) {
                return 1;
            }
            /* A socket handed in as a CRT descriptor is FILE_TYPE_PIPE too,
             * and PeekNamedPipe refuses it. */
            return le == ERROR_INVALID_FUNCTION || le == ERROR_INVALID_PARAMETER ? IPC_WAIT_NOT_PIPE
                                                                                 : IPC_WAIT_FAILED;
        }
        if (avail > 0) {
            if (avail_out) *avail_out = avail;
            return 1;
        }
        if (end >= 0) {
            long long left = end - ipc_now_ms();
            if (left <= 0) return 0;
            if ((long long)nap > left) nap = (DWORD)left;
        }
        Sleep(nap);
        if (nap < 8) nap *= 2;
    }
}

static const char* ipc_wait_msg(const char* what, int r) {
    static char msg[160];
    const char* why = r == IPC_WAIT_BAD_FD ? "invalid descriptor"
                    : r == IPC_WAIT_NOT_PIPE ? IPC_NOT_A_PIPE
                    : "PeekNamedPipe failed";
    snprintf(msg, sizeof msg, "%s: %s", what, why);
    return msg;
}

#define IPC_FILL_OK 0
#define IPC_FILL_EOF 1
#define IPC_FILL_TIMEOUT 2
#define IPC_FILL_ERR 3

/* An *err below 0 is an ipc_pipe_wait answer, not an errno. */
static int ipc_fill(int fd, char* dst, size_t len, long long deadline_ms, size_t* got, int* err) {
    *got = 0;
    ipc_binary(fd);
    while (*got < len) {
        size_t want = len - *got;
        if (deadline_ms >= 0) {
            long long left = deadline_ms - ipc_now_ms();
            DWORD avail = 0;
            int r = ipc_pipe_wait(fd, left < 0 ? 0 : left, &avail);
            if (r == 0) return IPC_FILL_TIMEOUT;
            if (r < 0) { *err = r; return IPC_FILL_ERR; }
            /* Never more than waits, so the read cannot block past the
             * deadline; 0 waiting is a closed writer, and the read says so. */
            if (avail > 0 && want > avail) want = avail;
        }
        int n = _read(fd, dst + *got, want > 0x7fffffffu ? 0x7fffffffu : (unsigned)want);
        if (n < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            return IPC_FILL_ERR;
        }
        if (n == 0) return IPC_FILL_EOF;
        *got += (size_t)n;
    }
    return IPC_FILL_OK;
}

_tuple_int_string ipc_wait_readable_raw(int ch, int timeout_ms) {
    if (ch < 0) return ipc_wait_failed("ipc.wait_readable: invalid descriptor");
    int r = ipc_pipe_wait(ch, timeout_ms, NULL);
    if (r < 0) return ipc_wait_failed(ipc_wait_msg("ipc.wait_readable", r));
    _tuple_int_string out = { r, "" };
    return out;
}

_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_failed("ipc.recv_fd: unsupported on Windows");
}

#endif /* AETHER_HAS_FILESYSTEM / _WIN32 */

#if AETHER_HAS_FILESYSTEM
/* Frames: a 4-byte big-endian unsigned length, then that many bytes, any
 * bytes. The reader takes exactly the header and then exactly the payload,
 * never a byte of the next frame, so it holds no state between calls and a
 * frame split at any offset across writes reads whole. The length is checked
 * against the caller's `max` before a byte is allocated for it. */
#define IPC_FRAME_HEADER 4

const char* ipc_write_frame_raw(int ch, const void* data) {
    if (ch < 0) return "ipc.write_frame: invalid descriptor";
    size_t len = 0;
    const char* bytes = ipc_bytes(data, &len);
    if (len > 0xffffffffu) return "ipc.write_frame: payload longer than a frame can announce (4 GiB - 1)";
    /* One buffer, one write: header and payload cannot be split by another
     * writer's bytes between them. */
    unsigned char* buf = (unsigned char*)malloc(len + IPC_FRAME_HEADER);
    if (!buf) return "ipc.write_frame: out of memory";
    buf[0] = (unsigned char)(len >> 24);
    buf[1] = (unsigned char)(len >> 16);
    buf[2] = (unsigned char)(len >> 8);
    buf[3] = (unsigned char)len;
    if (len) memcpy(buf + IPC_FRAME_HEADER, bytes, len);
    const char* err = ipc_write_all(ch, (const char*)buf, len + IPC_FRAME_HEADER, "ipc.write_frame");
    free(buf);
    return err;
}

/* The one frame reader. timeout_ms < 0 waits as long as the peer takes;
 * otherwise it bounds the whole frame, header and payload.
 *
 * A timeout has two answers, so a caller can tell them apart without a
 * poll of its own first. Before the first header byte nothing was consumed:
 * the stream is between frames and fit to use (send a CANCEL, read again).
 * Once a byte of the frame has been read, the bytes already taken are gone
 * with the call (the reader keeps no state between calls), so the stream is
 * out of step and the only safe thing is to close it. Keeping the partial
 * frame for the next call instead would need per-descriptor state the
 * runtime would have to find, size and free, and a caller that then wrote
 * to the peer would still be talking across a half-read frame. The strings
 * are std.ipc's TIMED_OUT_BETWEEN_FRAMES and TIMED_OUT_INSIDE_FRAME. */
#define IPC_TIMED_OUT_BETWEEN "ipc.read_frame: timed out before a frame began"
#define IPC_TIMED_OUT_INSIDE "ipc.read_frame: timed out inside a frame; the stream is out of step"

/* ipc_fill's error: an errno, or (Windows) an ipc_pipe_wait answer. */
static const char* ipc_fill_msg(int e) {
    if (e == ENOSYS) return "ipc.read_frame: a timeout is unavailable here";
#ifdef _WIN32
    if (e < 0) return ipc_wait_msg("ipc.read_frame", e);
#endif
    return ipc_errno_msg("ipc.read_frame", e);
}
_tuple_string_string ipc_read_frame_raw(int ch, int max, int timeout_ms) {
    if (ch < 0) return ipc_read_failed("ipc.read_frame: invalid descriptor");
    if (max <= 0) return ipc_read_failed("ipc.read_frame: max must be positive");
    long long deadline = timeout_ms < 0 ? -1 : ipc_now_ms() + timeout_ms;

    unsigned char hdr[IPC_FRAME_HEADER];
    size_t got = 0;
    int e = 0;
    int st = ipc_fill(ch, (char*)hdr, sizeof hdr, deadline, &got, &e);
    if (st == IPC_FILL_EOF) {
        return ipc_read_failed(got == 0 ? "ipc.read_frame: end of stream"
                                        : "ipc.read_frame: stream ended inside a frame header");
    }
    if (st == IPC_FILL_TIMEOUT) {
        return ipc_read_failed(got == 0 ? IPC_TIMED_OUT_BETWEEN : IPC_TIMED_OUT_INSIDE);
    }
    if (st == IPC_FILL_ERR) return ipc_read_failed(ipc_fill_msg(e));
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if ((uint64_t)len > (uint64_t)max) {
        return ipc_read_failed("ipc.read_frame: frame longer than max");
    }
    if (len == 0) {
        _tuple_string_string out = { (const char*)string_empty(), "" };
        return out;
    }
    char* buf = (char*)malloc(len);
    if (!buf) return ipc_read_failed("ipc.read_frame: out of memory");
    st = ipc_fill(ch, buf, len, deadline, &got, &e);
    if (st != IPC_FILL_OK) {
        free(buf);
        if (st == IPC_FILL_EOF) return ipc_read_failed("ipc.read_frame: stream ended inside a frame");
        if (st == IPC_FILL_TIMEOUT) return ipc_read_failed(IPC_TIMED_OUT_INSIDE);
        return ipc_read_failed(ipc_fill_msg(e));
    }
    AetherString* s = string_new_with_length(buf, len);
    free(buf);
    if (!s) return ipc_read_failed("ipc.read_frame: out of memory");
    _tuple_string_string out = { (const char*)s, "" };
    return out;
}
#endif /* AETHER_HAS_FILESYSTEM */
