/* aether_ipc.c — child-side IPC primitives, two-way socket channels, and descriptor passing. */

#include "../../runtime/config/aether_optimization_config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../string/aether_string.h"

typedef struct { const char* _0; const char* _1; } _tuple_string_string;
typedef struct { int _0; const char* _1; const char* _2; } _tuple_int_string_string;

#if !AETHER_HAS_FILESYSTEM

int ipc_parent_channel_raw(void) { return -1; }
const char* ipc_parent_channel_kind_raw(void) { return ""; }
const char* ipc_write_raw(int fd, const void* bytes_obj) { (void)fd; (void)bytes_obj; return "ipc.write unavailable"; }
_tuple_string_string ipc_read_raw(int fd, int max) {
    (void)fd; (void)max;
    _tuple_string_string out = { (const char*)string_empty(), "ipc.read unavailable" };
    return out;
}
const char* ipc_send_fd_raw(int ch, int fd, const void* payload_obj) {
    (void)ch; (void)fd; (void)payload_obj;
    return "ipc.send_fd unavailable";
}
_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    (void)ch; (void)max;
    _tuple_int_string_string out = { -1, (const char*)string_empty(), "ipc.recv_fd unavailable" };
    return out;
}
int ipc_set_sock_buf_raw(int fd, int size) { (void)fd; (void)size; return -1; }
int ipc_get_cloexec_raw(int fd) { (void)fd; return -1; }
int test_ipc_send_two_fds(int ch, int fd1, int fd2, const void* payload_obj) { (void)ch; (void)fd1; (void)fd2; (void)payload_obj; return -1; }
int ipc_test_send_async_raw(int fd, const void* payload_obj) { (void)fd; (void)payload_obj; return -1; }

#else  /* AETHER_HAS_FILESYSTEM */

#ifndef _WIN32

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

int ipc_parent_channel_raw(void) {
    const char* s = getenv("AETHER_IPC_FD");
    if (!s || !s[0]) return -1;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    if (v < 0 || v > 65535) return -1;
    int fd = (int)v;
    if (fcntl(fd, F_GETFD) < 0) return -1;
    return fd;
}

const char* ipc_parent_channel_kind_raw(void) {
    int fd = ipc_parent_channel_raw();
    if (fd < 0) return "";
    const char* k = getenv("AETHER_IPC_KIND");
    if (k && strcmp(k, "socket") == 0) return "socket";
    return "pipe";
}

static const char* extract_bytes(const void* obj, size_t* len) {
    if (!obj) {
        *len = 0;
        return "";
    }
    if (aether_string_is_string(obj)) {
        AetherString* s = (AetherString*)obj;
        *len = s->length;
        return s->data;
    }
    *len = strlen((const char*)obj);
    return (const char*)obj;
}

const char* ipc_write_raw(int fd, const void* bytes_obj) {
    if (fd < 0) return "ipc.write: invalid file descriptor";
    size_t len = 0;
    const char* data = extract_bytes(bytes_obj, &len);

    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, data + total, len - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOTSOCK) {
                /* Fallback to write for plain pipes */
                n = write(fd, data + total, len - total);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    static char errbuf[256];
                    snprintf(errbuf, sizeof(errbuf), "ipc.write: write failed: %s", strerror(errno));
                    return errbuf;
                }
            } else {
                static char errbuf[256];
                snprintf(errbuf, sizeof(errbuf), "ipc.write: send failed: %s", strerror(errno));
                return errbuf;
            }
        }
        if (n == 0) {
            return "ipc.write: short write (0 bytes written)";
        }
        total += (size_t)n;
    }
    return "";
}

_tuple_string_string ipc_read_raw(int fd, int max) {
    _tuple_string_string out;
    out._0 = NULL;
    out._1 = "";

    if (fd < 0) {
        out._0 = (const char*)string_empty();
        out._1 = "ipc.read: invalid file descriptor";
        return out;
    }
    if (max <= 0) {
        out._0 = (const char*)string_empty();
        out._1 = "ipc.read: max bytes must be > 0";
        return out;
    }

    char* buf = (char*)malloc((size_t)max);
    if (!buf) {
        out._0 = (const char*)string_empty();
        out._1 = "ipc.read: out of memory";
        return out;
    }

    ssize_t n = 0;
    while (1) {
        n = read(fd, buf, (size_t)max);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            static char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "ipc.read: read failed: %s", strerror(errno));
            out._0 = (const char*)string_empty();
            out._1 = errbuf;
            return out;
        }
        break;
    }

    if (n == 0) {
        /* EOF — return empty string, empty error */
        free(buf);
        out._0 = (const char*)string_empty();
        out._1 = "";
        return out;
    }

    AetherString* s = (AetherString*)string_new_with_length(buf, (size_t)n);
    free(buf);
    if (!s) {
        out._0 = (const char*)string_empty();
        out._1 = "ipc.read: string allocation failed";
        return out;
    }

    out._0 = (const char*)s;
    out._1 = "";
    return out;
}

const char* ipc_send_fd_raw(int ch, int fd, const void* payload_obj) {
    if (ch < 0) return "ipc.send_fd: invalid channel descriptor";
    if (fd < 0) return "ipc.send_fd: invalid file descriptor to send";

    size_t payload_len = 0;
    const char* payload = extract_bytes(payload_obj, &payload_len);

    if (payload_len == 0) {
        return "ipc.send_fd: empty payload is refused";
    }

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    struct iovec iov;
    iov.iov_base = (void*)payload;
    iov.iov_len = payload_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char control_buf[CMSG_SPACE(sizeof(int))];
    memset(control_buf, 0, sizeof(control_buf));
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    while (1) {
        ssize_t n = sendmsg(ch, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            static char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "ipc.send_fd: sendmsg failed: %s", strerror(errno));
            return errbuf;
        }
        if ((size_t)n < payload_len) {
            return "ipc.send_fd: partial write during sendmsg";
        }
        break;
    }

    return "";
}

_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    _tuple_int_string_string out;
    out._0 = -1;
    out._1 = NULL;
    out._2 = "";

    if (ch < 0) {
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: invalid channel descriptor";
        return out;
    }
    if (max <= 0) {
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: max bytes must be > 0";
        return out;
    }

    char* payload_buf = (char*)malloc((size_t)max);
    if (!payload_buf) {
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: out of memory";
        return out;
    }

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    struct iovec iov;
    iov.iov_base = payload_buf;
    iov.iov_len = (size_t)max;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Allow buffer for up to 8 descriptors to detect multi-descriptor abuse */
    char control_buf[CMSG_SPACE(sizeof(int) * 8)];
    memset(control_buf, 0, sizeof(control_buf));
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);

    int recv_flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    recv_flags |= MSG_CMSG_CLOEXEC;
#endif

    ssize_t n = 0;
    while (1) {
        n = recvmsg(ch, &msg, recv_flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(payload_buf);
            static char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "ipc.recv_fd: recvmsg failed: %s", strerror(errno));
            out._1 = (const char*)string_empty();
            out._2 = errbuf;
            return out;
        }
        break;
    }

    if (n == 0) {
        free(payload_buf);
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: end of stream (peer closed)";
        return out;
    }

    /* Inspect control data */
    int received_fds[16];
    size_t num_fds = 0;
    int has_bad_cmsg = 0;

    if (msg.msg_flags & MSG_CTRUNC) {
        has_bad_cmsg = 1;
    }

    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            has_bad_cmsg = 1;
            continue;
        }
        size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
        size_t fds_in_cmsg = fd_bytes / sizeof(int);
        int* fds_ptr = (int*)(void*)CMSG_DATA(cmsg);
        for (size_t i = 0; i < fds_in_cmsg; i++) {
            if (num_fds < 16) {
                received_fds[num_fds++] = fds_ptr[i];
            } else {
                close(fds_ptr[i]);
            }
        }
    }

    if (has_bad_cmsg || (msg.msg_flags & MSG_CTRUNC)) {
        for (size_t i = 0; i < num_fds; i++) {
            close(received_fds[i]);
        }
        free(payload_buf);
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: control message truncated or invalid";
        return out;
    }

    if (num_fds == 0) {
        free(payload_buf);
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: message carried no file descriptor";
        return out;
    }

    if (num_fds > 1) {
        for (size_t i = 0; i < num_fds; i++) {
            close(received_fds[i]);
        }
        free(payload_buf);
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: more than one descriptor in one message";
        return out;
    }

    int recvd_fd = received_fds[0];

    /* Fallback set FD_CLOEXEC for systems lacking MSG_CMSG_CLOEXEC (e.g. macOS) */
    (void)fcntl(recvd_fd, F_SETFD, FD_CLOEXEC);

    AetherString* s = (AetherString*)string_new_with_length(payload_buf, (size_t)n);
    free(payload_buf);

    if (!s) {
        close(recvd_fd);
        out._1 = (const char*)string_empty();
        out._2 = "ipc.recv_fd: payload string allocation failed";
        return out;
    }

    out._0 = recvd_fd;
    out._1 = (const char*)s;
    out._2 = "";
    return out;
}

int ipc_set_sock_buf_raw(int fd, int size) {
    if (fd < 0 || size <= 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    return 0;
}

int ipc_get_cloexec_raw(int fd) {
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return (flags & FD_CLOEXEC) ? 1 : 0;
}

int test_ipc_send_two_fds(int ch, int fd1, int fd2, const void* payload_obj) {
    if (ch < 0 || fd1 < 0 || fd2 < 0) return -1;
    size_t payload_len = 0;
    const char* payload = extract_bytes(payload_obj, &payload_len);
    if (payload_len == 0) payload = "x", payload_len = 1;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    struct iovec iov;
    iov.iov_base = (void*)payload;
    iov.iov_len = payload_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char control_buf[CMSG_SPACE(sizeof(int) * 2)];
    memset(control_buf, 0, sizeof(control_buf));
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    int fds[2] = { fd1, fd2 };
    memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));

    ssize_t n = sendmsg(ch, &msg, MSG_NOSIGNAL);
    return (n > 0) ? 0 : -1;
}

typedef struct {
    int fd;
    AetherString* payload;
} AsyncSendArgs;

static void* async_send_thread(void* arg) {
    AsyncSendArgs* args = (AsyncSendArgs*)arg;
    ipc_write_raw(args->fd, args->payload);
    if (args->payload) string_free(args->payload);
    free(args);
    return NULL;
}

int ipc_test_send_async_raw(int fd, const void* payload_obj) {
    if (fd < 0) return -1;
    size_t len = 0;
    const char* data = extract_bytes(payload_obj, &len);
    AetherString* copy_str = (AetherString*)string_new_with_length(data, len);

    AsyncSendArgs* args = (AsyncSendArgs*)malloc(sizeof(AsyncSendArgs));
    if (!args) return -1;
    args->fd = fd;
    args->payload = copy_str;

    pthread_t th;
    if (pthread_create(&th, NULL, async_send_thread, args) != 0) {
        free(args);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

#else  /* _WIN32 */

int ipc_parent_channel_raw(void) { return -1; }
const char* ipc_parent_channel_kind_raw(void) { return ""; }
const char* ipc_write_raw(int fd, const void* bytes_obj) { (void)fd; (void)bytes_obj; return "ipc.write unsupported on Windows"; }
_tuple_string_string ipc_read_raw(int fd, int max) {
    (void)fd; (void)max;
    _tuple_string_string out = { (const char*)string_empty(), "ipc.read unsupported on Windows" };
    return out;
}
const char* ipc_send_fd_raw(int ch, int fd, const void* payload_obj) {
    (void)ch; (void)fd; (void)payload_obj;
    return "ipc.send_fd unsupported on Windows";
}
_tuple_int_string_string ipc_recv_fd_raw(int ch, int max) {
    (void)ch; (void)max;
    _tuple_int_string_string out = { -1, (const char*)string_empty(), "ipc.recv_fd unsupported on Windows" };
    return out;
}
int ipc_set_sock_buf_raw(int fd, int size) { (void)fd; (void)size; return -1; }
int ipc_get_cloexec_raw(int fd) { (void)fd; return -1; }
int test_ipc_send_two_fds(int ch, int fd1, int fd2, const void* payload_obj) { (void)ch; (void)fd1; (void)fd2; (void)payload_obj; return -1; }
int ipc_test_send_async_raw(int fd, const void* payload_obj) { (void)fd; (void)payload_obj; return -1; }

#endif  /* _WIN32 */

#endif /* AETHER_HAS_FILESYSTEM */
