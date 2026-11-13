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
 * carries AETHER_IPC_KIND=stdio, and no AETHER_IPC_FD). stdio_channel()
 * claims that pair for frames: it moves fd 0 and fd 1 to fresh close-on-exec
 * descriptors, then points fd 0 at the null device and fd 1 at stderr, so a
 * stray print or a library reading stdin cannot touch the channel. */

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
typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;

static _tuple_int_int_string ipc_stdio_failed(const char* err) {
    _tuple_int_int_string out = { -1, -1, err };
    return out;
}

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
_tuple_int_string ipc_wait_writable_raw(int ch, int timeout_ms) {
    (void)ch; (void)timeout_ms;
    return ipc_wait_failed("ipc.wait_writable: unavailable in this build");
}
_tuple_int_string ipc_write_some_raw(int fd, const void* bytes, int offset) {
    (void)fd; (void)bytes; (void)offset;
    return ipc_wait_failed("ipc.write_some: unavailable in this build");
}
void* ipc_frame_writer_new_raw(int fd) { (void)fd; return NULL; }
const char* ipc_frame_writer_put_raw(void* w, const void* data) {
    (void)w; (void)data;
    return "ipc.frame_writer_put: unavailable in this build";
}
const char* ipc_frame_writer_flush_raw(void* w, int timeout_ms) {
    (void)w; (void)timeout_ms;
    return "ipc.frame_writer_flush: unavailable in this build";
}
long long ipc_frame_writer_pending_raw(void* w) { (void)w; return 0; }
void ipc_frame_writer_free_raw(void* w) { (void)w; }
_tuple_int_int_string ipc_stdio_channel_raw(void) {
    return ipc_stdio_failed("ipc.stdio_channel: unavailable in this build");
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

/* macOS raises a write's SIGPIPE on the PROCESS, not the writing thread
 * (xnu's dofilewrite, for a pipe; a socket without SO_NOSIGPIPE likewise),
 * so the thread mask below holds it off only while no other thread leaves
 * SIGPIPE unblocked: a worker writing a child's stdin while the main thread
 * waited took the whole process down with 141 (nib-code ask
 * ipc-write-sigpipe). The descriptor's own F_SETNOSIGPIPE (SO_NOSIGPIPE, for
 * a socket) refuses the signal for every write to it, so each write std.ipc
 * makes sets it first. That is the descriptor's state, not the process's:
 * no handler or disposition changes, and other descriptors still raise
 * SIGPIPE as their owners expect. It stays set (restoring it after the call
 * would race another thread writing the same descriptor). */
static void ipc_fd_nosigpipe(int fd) {
#if defined(__APPLE__) && defined(F_SETNOSIGPIPE)
    (void)fcntl(fd, F_SETNOSIGPIPE, 1);
#else
    (void)fd;
#endif
}

#if IPC_HAS_SOCKETS
/* write(2) to a pipe with SIGPIPE held off this thread: a reader that has
 * gone is EPIPE, not a signal that ends the process (a pipe has no
 * MSG_NOSIGNAL or SO_NOSIGPIPE). SIGPIPE is blocked for the call; when the
 * write raised one that was not already pending, it is taken off again with
 * sigwait (it is pending, so that returns at once) before the old mask comes
 * back. A SIGPIPE pending before the call is left for its owner. Linux and
 * the BSDs raise it on the writing thread, which the mask covers; macOS
 * raises it on the process, which ipc_fd_nosigpipe covers. */
static ssize_t ipc_write_nosigpipe(int fd, const void* buf, size_t len) {
    ipc_fd_nosigpipe(fd);
    sigset_t pipe_only, old, pending;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    sigemptyset(&pending);
    sigpending(&pending);
    int was_pending = sigismember(&pending, SIGPIPE) == 1;
    if (pthread_sigmask(SIG_BLOCK, &pipe_only, &old) != 0) return write(fd, buf, len);
    ssize_t n = write(fd, buf, len);
    int e = errno;
    /* Not only on EPIPE: a write the reader's exit interrupted returns what
     * it had written so far and still raises SIGPIPE (Linux pipe_write),
     * which the old mask would then deliver. Any SIGPIPE this call made is
     * taken off. */
    if (!was_pending) {
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
    ipc_fd_nosigpipe(fd);
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
    ipc_fd_nosigpipe(ch);
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

#if IPC_HAS_SOCKETS
#include <limits.h>
#ifndef PIPE_BUF
#define PIPE_BUF 512
#endif

/* poll(2) for room to write for up to `ms` (negative: forever). 1 writable
 * (or errored or hung up: the write then says why), 0 timed out, -1 failed
 * with errno set. */
static int ipc_poll_out(int fd, long long ms) {
    long long end = ms < 0 ? -1 : ipc_now_ms() + ms;
    for (;;) {
        int wait = -1;
        if (end >= 0) {
            long long left = end - ipc_now_ms();
            wait = left <= 0 ? 0 : (left > 0x7fffffff ? 0x7fffffff : (int)left);
        }
        struct pollfd p;
        p.fd = fd;
        p.events = POLLOUT;
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

/* Write what `fd` takes now of `len` bytes, never waiting for room, and
 * never changing the descriptor's flags (another holder of the same open
 * pipe would see them). *done is how many went. A socket is sent to with
 * MSG_DONTWAIT. A pipe, or anything else, is written PIPE_BUF bytes at a
 * time while poll() says there is room: a blocking write of up to PIPE_BUF
 * bytes to a pipe with room does not wait (Linux keeps a free page then, the
 * BSDs and macOS at least PIPE_BUF bytes), where a larger one would wait for
 * the rest. A descriptor its owner made non-blocking is written straight.
 * MSG_DONTWAIT is used only where it keeps a send from waiting, Linux and
 * FreeBSD: macOS sends what fits and then waits for room for the rest (up
 * to the send timeout), and NetBSD ignores it on a TCP socket (std.net's
 * #3108 note), so elsewhere a socket goes the poll way too, where room for
 * PIPE_BUF bytes is room for the send (a socket is writable once its
 * low-water mark, 2048 bytes by default, is free). "" or the error; *done
 * counts the bytes that went before it. */
static const char* ipc_write_nowait(int fd, const char* data, size_t len, size_t* done,
                                    const char* what) {
    *done = 0;
    if (len == 0) return "";
#if defined(__linux__) || defined(__FreeBSD__)
    for (;;) {
        ssize_t n = send(fd, data + *done, len - *done, MSG_DONTWAIT | IPC_NOSIGNAL);
        if (n > 0) {
            *done += (size_t)n;
            if (*done == len) return "";
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return "";
        if (n < 0 && errno == ENOTSOCK) break;
        if (n == 0) return "";
        return ipc_errno_msg(what, errno);
    }
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return ipc_errno_msg(what, errno);
    int nonblocking = (flags & O_NONBLOCK) != 0;
    while (*done < len) {
        size_t want = len - *done;
        if (!nonblocking) {
            int r = ipc_poll_out(fd, 0);
            if (r < 0) return ipc_errno_msg(what, errno);
            if (r == 0) return "";
            if (want > PIPE_BUF) want = PIPE_BUF;
        }
        ssize_t n = ipc_write_nosigpipe(fd, data + *done, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return "";
            return ipc_errno_msg(what, errno);
        }
        if (n == 0) return "";
        *done += (size_t)n;
    }
    return "";
}

/* For the frame writer's flush: wait up to `ms` for room. 1 room (or an
 * error the write will report), 0 none by then, -1 failed (errno). */
static int ipc_room_wait(int fd, long long ms, unsigned* nap) {
    (void)nap;
    return ipc_poll_out(fd, ms);
}

_tuple_int_string ipc_wait_writable_raw(int ch, int timeout_ms) {
    if (ch < 0) return ipc_wait_failed("ipc.wait_writable: invalid descriptor");
    int r = ipc_poll_out(ch, timeout_ms);
    if (r < 0) return ipc_wait_failed(ipc_errno_msg("ipc.wait_writable", errno));
    _tuple_int_string out = { r, "" };
    return out;
}
#else
static const char* ipc_write_nowait(int fd, const char* data, size_t len, size_t* done,
                                    const char* what) {
    (void)fd; (void)data; (void)len; (void)what;
    *done = 0;
    return "a write that does not wait is unavailable in this build (no poll)";
}
static int ipc_room_wait(int fd, long long ms, unsigned* nap) {
    (void)fd; (void)ms; (void)nap;
    errno = ENOSYS;
    return -1;
}
_tuple_int_string ipc_wait_writable_raw(int ch, int timeout_ms) {
    (void)ch; (void)timeout_ms;
    return ipc_wait_failed("ipc.wait_writable: unavailable in this build (no sockets)");
}
#endif

#if IPC_HAS_SOCKETS
/* Set once stdio_channel has claimed fd 0 and fd 1: a second claim would
 * take the null device and stderr for a channel. */
static int ipc_stdio_claimed = 0;

/* ipc.stdio_channel: fd 0 and fd 1 become the channel (two fresh
 * close-on-exec descriptors, numbered 3 or above); fd 0 then reads the null
 * device and fd 1 writes where fd 2 does (the null device when fd 2 is not
 * open). Each step that fails puts back what the steps before it changed. */
_tuple_int_int_string ipc_stdio_channel_raw(void) {
    if (__atomic_exchange_n(&ipc_stdio_claimed, 1, __ATOMIC_ACQ_REL)) {
        return ipc_stdio_failed("ipc.stdio_channel: already claimed");
    }
    /* Whatever was printed before the claim goes out ahead of the first
     * frame, not after it into stderr. */
    fflush(stdout);
    int in = fcntl(0, F_DUPFD_CLOEXEC, 3);
    if (in < 0) {
        int e = errno;
        __atomic_store_n(&ipc_stdio_claimed, 0, __ATOMIC_RELEASE);
        return ipc_stdio_failed(ipc_errno_msg("ipc.stdio_channel: stdin", e));
    }
    int out = fcntl(1, F_DUPFD_CLOEXEC, 3);
    if (out < 0) {
        int e = errno;
        close(in);
        __atomic_store_n(&ipc_stdio_claimed, 0, __ATOMIC_RELEASE);
        return ipc_stdio_failed(ipc_errno_msg("ipc.stdio_channel: stdout", e));
    }
    int err_open = fcntl(2, F_GETFD) >= 0;
    int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
    /* With fd 2 closed the null device lands there: it stays as stderr. */
    if (nul >= 0 && nul <= 2) fcntl(nul, F_SETFD, 0);
    int e = 0;
    if (nul < 0) {
        e = errno;
    } else if (dup2(nul, 0) < 0) {
        e = errno;
    } else if (dup2(err_open ? 2 : nul, 1) < 0) {
        e = errno;
        dup2(in, 0);   /* put stdin back */
    }
    if (nul > 2) close(nul);
    if (e) {
        close(in);
        close(out);
        __atomic_store_n(&ipc_stdio_claimed, 0, __ATOMIC_RELEASE);
        return ipc_stdio_failed(ipc_errno_msg("ipc.stdio_channel", e));
    }
    /* C stdio's stdin may hold bytes it read ahead; they were the channel's
     * and are gone with the claim. Clear its end-of-file and error state so
     * a later read sees the null device. */
    clearerr(stdin);
    /* A child of this process is no longer on the channel. */
    if (ipc_env_says_stdio()) unsetenv("AETHER_IPC_KIND");
    _tuple_int_int_string r = { in, out, "" };
    return r;
}
#else
_tuple_int_int_string ipc_stdio_channel_raw(void) {
    return ipc_stdio_failed("ipc.stdio_channel: unavailable in this build (no processes)");
}
#endif

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
     * channel (os.run_stdio, stdio_channel). */
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

/* Writing without waiting, on Windows.
 *
 * An anonymous pipe (os.run_stdio's, or a child's stdin and stdout) has no
 * readiness to wait on and cannot be opened for overlapped I/O after the
 * fact, and the child end of a channel is a pipe someone else made. Two
 * things every pipe handle does give, probed on win11:
 *
 * - its free room, WriteQuotaAvailable (NtQueryInformationFile,
 *   FilePipeLocalInformation). A blocking WriteFile of no more than that
 *   is buffered at once, so it does not wait. Windows takes a waiting
 *   reader's read off that room, though: a reader blocked in a read as
 *   large as the pipe's buffer (4096 bytes for os.run_stdio's) makes an
 *   empty pipe report none;
 * - PIPE_NOWAIT (SetNamedPipeHandleState), under which a WriteFile never
 *   waits. It is all or nothing past the room (a 200000-byte write to an
 *   empty 4096-byte pipe wrote 0 bytes, a 1000-byte one all of it, one to a
 *   full pipe ERROR_PIPE_BUSY), and the mode can be set only while the pipe
 *   holds no bytes: with 4000 bytes queued, setting it failed with
 *   ERROR_PIPE_BUSY. A reader with a read waiting is given bytes straight
 *   away under it.
 *
 * So a write that must not wait writes what the room says, with the pipe
 * left as it is; and when the room says none, it tries PIPE_NOWAIT for its
 * own WriteFile calls (halving a refused write, down to one byte), which
 * the pipe takes only when it is empty, which is when a reader may be
 * waiting, and puts PIPE_WAIT back before returning. Nothing is left
 * changed, but a blocking writer on another thread at the same moment would
 * see the no-wait mode: do not share the descriptor between threads.
 *
 * wait_writable is the room: it can time out on that empty pipe whose
 * reader waits in a large read, while a write would go. The frame writer
 * does not rely on it: it tries the write itself, a timer tick apart. */
typedef struct {
    ULONG NamedPipeType, NamedPipeConfiguration, MaximumInstances, CurrentInstances,
          InboundQuota, ReadDataAvailable, OutboundQuota, WriteQuotaAvailable,
          NamedPipeState, NamedPipeEnd;
} IpcPipeLocalInfo;
typedef struct { union { LONG Status; PVOID Pointer; } u; ULONG_PTR Information; } IpcIoStatus;
typedef LONG (WINAPI *IpcQueryInfoFile)(HANDLE, IpcIoStatus*, PVOID, ULONG, ULONG);
#define IPC_FILE_PIPE_LOCAL_INFORMATION 24
#define IPC_PIPE_CLOSING_STATE 4

/* Room in the pipe under `h`: its WriteQuotaAvailable in *room, and 1 in
 * *closing when the reader has gone. 0, or -1 when it cannot be asked. */
static int ipc_pipe_room(HANDLE h, DWORD* room, int* closing) {
    static IpcQueryInfoFile query = NULL;
    if (!query) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        query = nt ? (IpcQueryInfoFile)(void*)GetProcAddress(nt, "NtQueryInformationFile") : NULL;
        if (!query) return -1;
    }
    IpcIoStatus io;
    IpcPipeLocalInfo info;
    memset(&info, 0, sizeof info);
    if (query(h, &io, &info, sizeof info, IPC_FILE_PIPE_LOCAL_INFORMATION) < 0) return -1;
    *room = info.WriteQuotaAvailable;
    *closing = info.NamedPipeState == IPC_PIPE_CLOSING_STATE;
    return 0;
}

/* "<what>: <why>" for a Win32 error from a pipe write. */
static const char* ipc_win_write_msg(const char* what, DWORD le) {
    static char msg[160];
    if (le == ERROR_NO_DATA || le == ERROR_BROKEN_PIPE) {
        snprintf(msg, sizeof msg, "%s: the reader has gone (broken pipe)", what);
    } else {
        snprintf(msg, sizeof msg, "%s: WriteFile failed (Windows error %lu)", what, (unsigned long)le);
    }
    return msg;
}

/* A no-wait write of up to `len` bytes to an empty pipe in PIPE_WAIT
 * mode: PIPE_NOWAIT for the WriteFile calls, halving a refused write, then
 * PIPE_WAIT again. *wrote is what went; "" when the pipe would not take the
 * mode (it holds bytes: it is full, not waited on), or the error. */
static const char* ipc_pipe_try_nowait(HANDLE h, DWORD state, const char* data, size_t len,
                                       size_t* wrote, const char* what) {
    *wrote = 0;
    int set = !(state & PIPE_NOWAIT);
    if (set) {
        DWORD mode = (state & PIPE_READMODE_MESSAGE) | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(h, &mode, NULL, NULL)) {
            DWORD le = GetLastError();
            return le == ERROR_PIPE_BUSY ? "" : ipc_win_write_msg(what, le);
        }
    }
    const char* err = "";
    size_t chunk = len > 0x40000000u ? 0x40000000u : len;
    while (*wrote < len) {
        if (chunk > len - *wrote) chunk = len - *wrote;
        DWORD n = 0;
        if (!WriteFile(h, data + *wrote, (DWORD)chunk, &n, NULL)) {
            DWORD le = GetLastError();
            if (le != ERROR_PIPE_BUSY) {
                err = ipc_win_write_msg(what, le);
                break;
            }
            n = 0;
        }
        if (n > 0) {
            *wrote += n;
            continue;
        }
        if (chunk == 1) break;          /* full */
        chunk /= 2;
    }
    if (set) {
        DWORD mode = (state & PIPE_READMODE_MESSAGE) | PIPE_WAIT;
        SetNamedPipeHandleState(h, &mode, NULL, NULL);
    }
    return err;
}

static const char* ipc_write_nowait(int fd, const char* data, size_t len, size_t* done,
                                    const char* what) {
    *done = 0;
    if (len == 0) return "";
    intptr_t osh = _get_osfhandle(fd);
    if (osh == -1 || osh == -2) return ipc_wait_msg(what, IPC_WAIT_BAD_FD);
    HANDLE h = (HANDLE)osh;
    if (GetFileType(h) != FILE_TYPE_PIPE) {
        /* A file or a console does not wait on a reader: written whole. */
        const char* err = ipc_write_all(fd, data, len, what);
        if (!*err) *done = len;
        return err;
    }
    DWORD state = 0;
    if (!GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL, 0)) {
        return ipc_wait_msg(what, IPC_WAIT_NOT_PIPE);   /* a socket, say */
    }
    while (*done < len) {
        DWORD room = 0;
        int closing = 0;
        if (ipc_pipe_room(h, &room, &closing) != 0) return ipc_wait_msg(what, IPC_WAIT_NOT_PIPE);
        if (closing) return ipc_win_write_msg(what, ERROR_NO_DATA);
        size_t rest = len - *done;
        if (room > 0) {
            /* Fits in the free room: buffered at once, in either mode. */
            DWORD want = rest < room ? (DWORD)rest : room;
            DWORD n = 0;
            if (!WriteFile(h, data + *done, want, &n, NULL)) {
                DWORD le = GetLastError();
                if (le == ERROR_PIPE_BUSY) break;   /* a no-wait handle, full after all */
                return ipc_win_write_msg(what, le);
            }
            if (n == 0) break;
            *done += n;
            continue;
        }
        /* No room: full, or a reader waits in a read larger than the room. */
        size_t wrote = 0;
        const char* err = ipc_pipe_try_nowait(h, state, data + *done, rest, &wrote, what);
        *done += wrote;
        if (*err) return err;
        if (wrote == 0) break;
    }
    return "";
}

/* For the frame writer's flush: a pipe has nothing to wait on (above), so
 * sleep a little, 1 ms doubling to 8 (about a timer tick), and let the
 * caller try the write again. 1 try again, 0 out of time. */
static int ipc_room_wait(int fd, long long ms, unsigned* nap) {
    (void)fd;
    if (ms == 0) return 0;
    DWORD d = *nap ? *nap : 1;
    if (ms > 0 && (long long)d > ms) d = (DWORD)ms;
    Sleep(d);
    if (*nap < 8) *nap = *nap ? *nap * 2 : 2;
    return 1;
}

_tuple_int_string ipc_wait_writable_raw(int ch, int timeout_ms) {
    if (ch < 0) return ipc_wait_failed("ipc.wait_writable: invalid descriptor");
    intptr_t osh = _get_osfhandle(ch);
    if (osh == -1 || osh == -2) return ipc_wait_failed("ipc.wait_writable: invalid descriptor");
    HANDLE h = (HANDLE)osh;
    DWORD type = GetFileType(h);
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_CHAR) {
        _tuple_int_string ready = { 1, "" };   /* nothing to wait for */
        return ready;
    }
    if (type != FILE_TYPE_PIPE) return ipc_wait_failed(ipc_wait_msg("ipc.wait_writable", IPC_WAIT_NOT_PIPE));
    long long end = timeout_ms < 0 ? -1 : ipc_now_ms() + timeout_ms;
    DWORD nap = 1;
    for (;;) {
        DWORD room = 0;
        int closing = 0;
        if (ipc_pipe_room(h, &room, &closing) != 0) {
            return ipc_wait_failed(ipc_wait_msg("ipc.wait_writable", IPC_WAIT_NOT_PIPE));
        }
        if (room > 0 || closing) {
            _tuple_int_string out = { 1, "" };
            return out;
        }
        if (end >= 0) {
            long long left = end - ipc_now_ms();
            if (left <= 0) {
                _tuple_int_string out = { 0, "" };
                return out;
            }
            if ((long long)nap > left) nap = (DWORD)left;
        }
        Sleep(nap);
        if (nap < 8) nap *= 2;
    }
}

_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    (void)ch; (void)max;
    return ipc_recv_failed("ipc.recv_fd: unsupported on Windows");
}

static volatile LONG ipc_stdio_claimed = 0;

/* A CRT descriptor of its own on a non-inheritable duplicate of `fd`'s
 * handle, in binary mode; -1 when `fd` has none. */
static int ipc_dup_private(int fd, int flags) {
    intptr_t osh = _get_osfhandle(fd);
    if (osh == -1 || osh == -2 || osh == 0) return -1;
    HANDLE dup = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)osh, GetCurrentProcess(), &dup,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return -1;
    }
    int nfd = _open_osfhandle((intptr_t)dup, flags | _O_BINARY);
    if (nfd < 0) CloseHandle(dup);
    return nfd;
}

/* Point CRT descriptor `target` (0 or 1) and its Win32 std handle at
 * `src`'s handle. _dup2 sets the std handle itself in a console program;
 * SetStdHandle makes sure of it, so GetStdHandle-based writers follow. */
static int ipc_redirect(int src, int target, DWORD which) {
    if (_dup2(src, target) != 0) return -1;
    intptr_t osh = _get_osfhandle(target);
    if (osh != -1 && osh != -2) SetStdHandle(which, (HANDLE)osh);
    return 0;
}

/* ipc.stdio_channel on Windows: the CRT's fd 0 and fd 1 and the Win32
 * standard handles alike. The channel is two non-inheritable duplicates of
 * fd 0's and fd 1's handles, as binary CRT descriptors; fd 0 and
 * STD_INPUT_HANDLE then read NUL, fd 1 and STD_OUTPUT_HANDLE write where
 * stderr does (NUL when there is no stderr). */
_tuple_int_int_string ipc_stdio_channel_raw(void) {
    if (InterlockedExchange(&ipc_stdio_claimed, 1)) {
        return ipc_stdio_failed("ipc.stdio_channel: already claimed");
    }
    fflush(stdout);
    int in = ipc_dup_private(0, _O_RDONLY);
    if (in < 0) {
        InterlockedExchange(&ipc_stdio_claimed, 0);
        return ipc_stdio_failed("ipc.stdio_channel: stdin has no handle");
    }
    int out = ipc_dup_private(1, _O_WRONLY);
    if (out < 0) {
        _close(in);
        InterlockedExchange(&ipc_stdio_claimed, 0);
        return ipc_stdio_failed("ipc.stdio_channel: stdout has no handle");
    }
    HANDLE old_in = GetStdHandle(STD_INPUT_HANDLE);
    int nul = _open("NUL", _O_RDWR | _O_BINARY | _O_NOINHERIT);
    /* stderr: fd 2's handle, else STD_ERROR_HANDLE's, else NUL. */
    int err = ipc_dup_private(2, _O_WRONLY);
    if (err < 0) {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        HANDLE dup = NULL;
        if (h && h != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            err = _open_osfhandle((intptr_t)dup, _O_WRONLY | _O_BINARY);
            if (err < 0) CloseHandle(dup);
        }
    }
    const char* why = NULL;
    if (nul < 0) {
        why = "ipc.stdio_channel: cannot open NUL";
    } else if (ipc_redirect(nul, 0, STD_INPUT_HANDLE) != 0) {
        why = "ipc.stdio_channel: cannot redirect stdin";
    } else if (ipc_redirect(err >= 0 ? err : nul, 1, STD_OUTPUT_HANDLE) != 0) {
        why = "ipc.stdio_channel: cannot redirect stdout";
        _dup2(in, 0);   /* put stdin back */
        SetStdHandle(STD_INPUT_HANDLE, old_in);
    }
    if (nul >= 0) _close(nul);
    if (err >= 0) _close(err);
    if (why) {
        _close(in);
        _close(out);
        InterlockedExchange(&ipc_stdio_claimed, 0);
        return ipc_stdio_failed(why);
    }
    /* fd 1 keeps the text mode stdout has by default: a stray line on stderr
     * reads as one there. */
    _setmode(1, _O_TEXT);
    clearerr(stdin);
    if (ipc_env_says_stdio()) {
        SetEnvironmentVariableW(L"AETHER_IPC_KIND", NULL);
        _putenv("AETHER_IPC_KIND=");
    }
    _tuple_int_int_string r = { in, out, "" };
    return r;
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

/* ipc.write_some: the bytes of `bytes` from `offset` on that `fd` takes now,
 * without waiting (ipc_write_nowait). (count, "") or (count, err), the count
 * being what went before the error. */
_tuple_int_string ipc_write_some_raw(int fd, const void* bytes, int offset) {
    _tuple_int_string out = { 0, "" };
    if (fd < 0) { out._1 = "ipc.write_some: invalid descriptor"; return out; }
    size_t len = 0;
    const char* data = ipc_bytes(bytes, &len);
    if (offset < 0 || (size_t)offset > len) { out._1 = "ipc.write_some: offset outside the bytes"; return out; }
    size_t rest = len - (size_t)offset;
    if (rest > 0x7fffffffu) rest = 0x7fffffffu;   /* the count is an int */
    size_t done = 0;
    out._1 = ipc_write_nowait(fd, data + offset, rest, &done, "ipc.write_some");
    out._0 = (int)done;
    return out;
}

/* A frame writer: frames queued in a buffer of its own and written as the
 * descriptor takes them, so a caller on a UI thread never waits on a reader.
 * Each writer holds its own queue and place; nothing is shared between
 * writers, and the descriptor stays the caller's (free does not close it). A
 * frame partly written by one flush carries on where it stopped in the next,
 * so the peer reads whole frames in order. One thread at a time. */
typedef struct {
    int fd;
    unsigned char* buf;   /* queued bytes: [off, len) are still to write */
    size_t off, len, cap;
} IpcFrameWriter;

void* ipc_frame_writer_new_raw(int fd) {
    if (fd < 0) return NULL;
    IpcFrameWriter* w = (IpcFrameWriter*)calloc(1, sizeof(IpcFrameWriter));
    if (!w) return NULL;
    w->fd = fd;
    return w;
}

void ipc_frame_writer_free_raw(void* p) {
    IpcFrameWriter* w = (IpcFrameWriter*)p;
    if (!w) return;
    free(w->buf);
    free(w);
}

long long ipc_frame_writer_pending_raw(void* p) {
    IpcFrameWriter* w = (IpcFrameWriter*)p;
    return w ? (long long)(w->len - w->off) : 0;
}

/* Queue `data` as one frame (header and payload, as write_frame writes
 * them). Writes nothing. */
const char* ipc_frame_writer_put_raw(void* p, const void* data) {
    IpcFrameWriter* w = (IpcFrameWriter*)p;
    if (!w) return "ipc.frame_writer_put: no writer";
    size_t len = 0;
    const char* bytes = ipc_bytes(data, &len);
    if (len > 0xffffffffu) return "ipc.frame_writer_put: payload longer than a frame can announce (4 GiB - 1)";
    if (w->off > 0 && w->off == w->len) w->off = w->len = 0;
    size_t need = len + IPC_FRAME_HEADER;
    if (w->len + need > w->cap) {
        /* What has been written goes first, then the buffer grows. */
        if (w->off > 0) {
            memmove(w->buf, w->buf + w->off, w->len - w->off);
            w->len -= w->off;
            w->off = 0;
        }
        if (w->len + need > w->cap) {
            size_t cap = w->cap ? w->cap : 256;
            while (cap < w->len + need) {
                if (cap > ((size_t)-1) / 2) return "ipc.frame_writer_put: out of memory";
                cap *= 2;
            }
            unsigned char* nb = (unsigned char*)realloc(w->buf, cap);
            if (!nb) return "ipc.frame_writer_put: out of memory";
            w->buf = nb;
            w->cap = cap;
        }
    }
    unsigned char* h = w->buf + w->len;
    h[0] = (unsigned char)(len >> 24);
    h[1] = (unsigned char)(len >> 16);
    h[2] = (unsigned char)(len >> 8);
    h[3] = (unsigned char)len;
    if (len) memcpy(h + IPC_FRAME_HEADER, bytes, len);
    w->len += need;
    return "";
}

/* Write what is queued. timeout_ms 0: what the descriptor takes now, never
 * waiting; above 0: wait for room up to that long in all; below 0: until
 * everything is written. "" when it is written or the time ran out (the
 * rest stays queued: frame_writer_pending), or the error that stopped it
 * (the peer gone, say), the rest still queued. */
const char* ipc_frame_writer_flush_raw(void* p, int timeout_ms) {
    IpcFrameWriter* w = (IpcFrameWriter*)p;
    if (!w) return "ipc.frame_writer_flush: no writer";
    long long end = timeout_ms < 0 ? -1 : ipc_now_ms() + timeout_ms;
    unsigned nap = 0;
    while (w->off < w->len) {
        size_t done = 0;
        const char* err = ipc_write_nowait(w->fd, (const char*)w->buf + w->off, w->len - w->off,
                                           &done, "ipc.frame_writer_flush");
        w->off += done;
        if (*err) return err;
        if (w->off == w->len) break;
        long long left = -1;
        if (end >= 0) {
            left = end - ipc_now_ms();
            if (left <= 0) break;
        }
        int r = ipc_room_wait(w->fd, left, &nap);
        if (r < 0) {
#ifdef _WIN32
            return "ipc.frame_writer_flush: cannot wait for room";
#else
            return ipc_errno_msg("ipc.frame_writer_flush", errno);
#endif
        }
        if (r == 0) break;
    }
    if (w->off == w->len) w->off = w->len = 0;
    return "";
}
#endif /* AETHER_HAS_FILESYSTEM */
