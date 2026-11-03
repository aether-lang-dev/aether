/* stunt_main.c: the stunt program (see README.md).
 *
 *   stunt list                          the catalogue
 *   stunt <check> <succeed|refused>     run one check; host-checked ones take "host"
 *
 * Prints one line and exits 0 when the outcome matched the expectation, 1
 * when it did not, 2 on a usage error (3: a host-checked check was stopped).
 *
 * Run by a host with a socket (sandbox.spawn_socket or os.run_socket, which
 * set AETHER_IPC_FD), it is the engine's child-process mode: it speaks
 * std.ipc's frames (a 4-byte big-endian length, then the payload) on that
 * descriptor, and sends its line to the host instead of printing it.
 *
 *   child -> host   CALL <name>[\t<arg>]...   an argument is s<text> or i<int>
 *   host -> child   OK s<text> | OK i<int> | OK n | ERR <reason>
 *   host -> child   CANCEL                     at any time; stops spin
 *   child -> host   RESULT <code> <line>       last, then the child exits <code>
 *
 * Every message is written whole as one frame, except by half-frame, which
 * stops part-way through one on purpose.
 */
#include "stunt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#define FRAME_MAX 65536

static int g_ch = -1;
static int g_cancel_seen = 0;

static int write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, char* p, size_t n) {
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_frame(const char* data, size_t n) {
    unsigned char h[4] = {
        (unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n
    };
    if (write_all(g_ch, (const char*)h, 4) != 0) return -1;
    return write_all(g_ch, data, n);
}

/* One frame into a malloc'd, NUL-terminated buffer; NULL at end or error. */
static char* read_frame(void) {
    unsigned char h[4];
    if (read_all(g_ch, (char*)h, 4) != 0) return NULL;
    size_t n = ((size_t)h[0] << 24) | ((size_t)h[1] << 16) | ((size_t)h[2] << 8) | (size_t)h[3];
    if (n > FRAME_MAX) return NULL;
    char* buf = (char*)malloc(n + 1);
    if (!buf) return NULL;
    if (n > 0 && read_all(g_ch, buf, n) != 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* Whether the host has hung up: the socket polls as closed, or reads EOF.
 * Whatever else waits there is read and dropped. Waits up to `ms`. */
static int hung_up(int ms) {
    struct pollfd p;
    p.fd = g_ch;
    p.events = POLLIN;
    p.revents = 0;
    int r = poll(&p, 1, ms);
    if (r < 0) return errno != EINTR;
    if (r == 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return 1;
    char buf[256];
    ssize_t n = read(g_ch, buf, sizeof buf);
    return n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN);
}

/* half-frame: a header announcing 64 bytes, 10 of them, then nothing more.
 * The channel is broken from here on, so the program ends: when the host
 * hangs up, which a host that timed out inside the frame does, or after
 * ST_SILENT_CAP_MS if it never does. */
static void stall_mid_frame(void) {
    const unsigned char h[4] = { 0, 0, 0, 64 };
    if (write_all(g_ch, (const char*)h, 4) == 0) (void)write_all(g_ch, "CALL greet", 10);
    for (int waited = 0; waited < ST_SILENT_CAP_MS; waited += 100) {
        if (hung_up(100)) break;
    }
    _exit(ST_FAIL);
}

/* Every host function goes to the host: the fallback under "*". */
static int forward(void* vm) {
    const char* name = st_call_name(vm);
    if (strcmp(name, "__stall_mid_frame") == 0) stall_mid_frame();
    size_t cap = strlen(name) + 16;
    int n = st_arg_count(vm);
    for (int i = 1; i <= n; i++) cap += strlen(st_arg_str(vm, i)) + 32;
    char* msg = (char*)malloc(cap);
    if (!msg) return st_raise_error(vm, "out of memory");
    size_t len = (size_t)snprintf(msg, cap, "CALL %s", name);
    for (int i = 1; i <= n && len < cap; i++) {
        if (st_arg_type(vm, i) == ST_TINT) {
            len += (size_t)snprintf(msg + len, cap - len, "\ti%lld", (long long)st_arg_int(vm, i));
        } else {
            len += (size_t)snprintf(msg + len, cap - len, "\ts%s", st_arg_str(vm, i));
        }
    }
    if (len >= cap) len = cap - 1;
    int w = write_frame(msg, len);
    free(msg);
    if (w != 0) return st_raise_error(vm, "the host hung up");
    for (;;) {
        char* reply = read_frame();
        if (!reply) return st_raise_error(vm, "the host hung up");
        if (strcmp(reply, "CANCEL") == 0) {
            free(reply);
            g_cancel_seen = 1;
            return st_raise_error(vm, "cancelled by the host");
        }
        int r = 0;
        if (strncmp(reply, "OK s", 4) == 0) {
            st_push_str(vm, reply + 4);
            r = 1;
        } else if (strncmp(reply, "OK i", 4) == 0) {
            st_push_int(vm, strtoll(reply + 4, NULL, 10));
            r = 1;
        } else if (strcmp(reply, "OK n") == 0) {
            st_push_nil(vm);
            r = 1;
        } else if (strncmp(reply, "ERR ", 4) == 0) {
            r = st_raise_error(vm, reply + 4);
        } else {
            r = st_raise_error(vm, "the host's reply was not understood");
        }
        free(reply);
        return r;
    }
}

/* Called every ST_INTERRUPT_EVERY instructions of spin, and by silent as
 * it waits: a CANCEL waiting on the socket stops spin (silent ignores it),
 * and a host that hung up (POLLHUP, or EOF) stops both. Nothing else
 * arrives unasked. */
static int g_hung_up = 0;
static int interrupted(void* ud) {
    (void)ud;
    if (g_hung_up) return ST_INTERRUPT_HUNG_UP;
    struct pollfd p;
    p.fd = g_ch;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, 0) > 0) {
        char* f = read_frame();
        if (!f) {
            g_hung_up = 1;
            return ST_INTERRUPT_HUNG_UP;
        }
        if (strcmp(f, "CANCEL") == 0) g_cancel_seen = 1;
        free(f);
    }
    return g_cancel_seen;
}
#endif

static void list(void) {
    int n = st_catalogue_size();
    printf("%-26s %-17s %-12s %s\n", "CHECK", "MODES", "JUDGED BY", "WHAT IT DOES");
    for (int i = 0; i < n; i++) {
        printf("%-26s %-17s %-12s %s\n", st_catalogue_name(i), st_catalogue_modes(i),
               st_catalogue_host_checked(i) ? "host" : "stunt", st_catalogue_doc(i));
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        list();
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: stunt list\n       stunt <check> <succeed|refused|host>\n");
        return ST_USAGE;
    }
    st_vm* vm = st_new();
    if (!vm) {
        fprintf(stderr, "stunt: out of memory\n");
        return ST_FAIL;
    }
    int child = 0;
#if !defined(_WIN32)
    const char* fdv = getenv("AETHER_IPC_FD");
    if (fdv && *fdv) {
        g_ch = atoi(fdv);
        child = 1;
        signal(SIGPIPE, SIG_IGN);
        alarm(ST_CHILD_CAP_S);
        st_register(vm, "*", forward);
        st_set_interrupt_handler(vm, interrupted, NULL);
    }
#endif
    /* Run by hand, nothing will cancel spin: it stops at a default limit.
     * Run by a host, the host's CANCEL is what has to stop it. */
    if (!child) st_set_instruction_limit(vm, ST_DEFAULT_BUDGET);
    size_t len = strlen(argv[1]) + strlen(argv[2]) + 2;
    char* source = (char*)malloc(len);
    if (!source) return ST_FAIL;
    snprintf(source, len, "%s %s", argv[1], argv[2]);
    int rc = st_eval(vm, source);
    free(source);
    if (child) {
#if !defined(_WIN32)
        char msg[1200];
        int n = snprintf(msg, sizeof msg, "RESULT %d %s", rc, st_result(vm));
        if (n < 0) n = 0;
        if ((size_t)n >= sizeof msg) n = (int)sizeof msg - 1;
        if (write_frame(msg, (size_t)n) != 0) fprintf(stderr, "%s\n", st_result(vm));
#endif
    } else {
        printf("%s\n", st_result(vm));
    }
    st_free(vm);
    return rc;
}
