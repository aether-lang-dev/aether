/* Two tasks entering one cooperative (AETHER_NO_THREADING) runtime at once,
 * each with its own execution context and its own execution account.
 *
 * POSIX threads stand in for FreeRTOS tasks: the runtime is built without
 * threads, as on an ESP32, and the threads here run truly at once, which
 * interleaves the two harder than one core's preemption does. The port the
 * host installs keeps each thread's context in a pthread key, as the
 * ESP-IDF port keeps it in a FreeRTOS thread-local pointer.
 *
 * The interrupted task (I) runs under a 64 KiB account and is driven, round
 * after round, past that allowance (AETHER_MEMORY_CAP), past a deadline
 * (AETHER_DEADLINE), into a cancel from a third thread (AETHER_CANCELLED)
 * and into a panic no try catches (AETHER_PANICKED), with its own caller
 * identity "I". The worker task (W) meanwhile makes ordinary calls under an
 * account of its own: each must return its value with AETHER_OK, leave
 * nothing charged to W's account, see W's identity, and a try in it must
 * catch its own panic. Any call of W's that comes back otherwise, or any of
 * I's that ends with another outcome than the one driven, is counted.
 *
 * Built with -DNO_CONTEXT_API, the host installs no port and attaches
 * nothing: both threads share the runtime's one built-in context, which is
 * what a cooperative build gave every task before task contexts. The test
 * script runs that build only to show the interference; the check is on the
 * build with contexts.
 *
 *   host all        both tasks, all four interruptions
 *   host deadline   only the deadline (no panic, so a run without contexts
 *                   shows the crossed deadline rather than a crash)
 *   host caller     only caller identities
 *   host churn      attach and detach repeatedly; a thread that ends
 *                   attached; a context's cancel cell reused, not leaked
 *   host unattached a thread that enters without attaching (must abort)
 *   host actors     the installing thread asks an actor; then an attached
 *                   thread that does not own the actors tries (must abort)
 *
 * Built with -DNET_MODES, against the runtime built with networking and a
 * filesystem (and the sandbox), the per-thread state of std and the runtime
 * a cooperative build used to keep in thread-locals, or in plain globals all
 * tasks shared:
 *
 *   host clock      one task pins std.net's millisecond clock and holds it
 *                   pinned; the other must read a running clock
 *   host sandbox    one task has a sandbox level in force that grants
 *                   nothing; the other must see none and be refused nothing
 *   host pool       two tasks make keep-alive requests at once through the
 *                   HTTP client's one connection pool, to a local server
 *                   that echoes each request's path; each response must be
 *                   the task's own, with its joined duplicate headers
 *   host tcpstop    a task blocked reading a silent peer under std.tcp's
 *                   deadline and cancel handle: a native task that never
 *                   enters Aether cancels it, then a deadline ends the next
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libaether.h"

extern int32_t aether_grow(int32_t n);
extern int32_t aether_spin(int32_t n);
extern int32_t aether_boom(int32_t n);
extern int32_t aether_caught(int32_t n);
extern const void* aether_who(void);
extern int32_t aether_ask_echo(int32_t n);
extern void aether_ctxlib_string_release(const void* s);
extern const char* aether_string_data(const void* s);
extern int aether_set_caller(const char* identity, const char** keys, const char** vals, size_t n,
                             int64_t deadline_ms);

#define ROUNDS 60

static _Atomic int g_i_done = 0;
static _Atomic int g_w_rounds = 0;
static _Atomic int g_w_bad = 0;
static _Atomic int g_i_bad = 0;
static char g_first_bad[512];
static atomic_flag g_first_lock = ATOMIC_FLAG_INIT;

static void note_bad(const char* who, const char* what, int got, int want) {
    if (!atomic_flag_test_and_set(&g_first_lock))
        snprintf(g_first_bad, sizeof g_first_bad, "%s: %s: got %d, want %d (outcome text \"%s\")", who, what,
                 got, want, aether_last_outcome_text());
}

/* A cancel request from the canceller thread: the token of I's spin. */
static _Atomic int g_cancel_ready = 0;
static AetherExecToken g_cancel_token;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#ifndef NO_CONTEXT_API
/* The port: a pthread key per thread, its destructor the runtime's own
 * ending of a context whose task ended attached. */
static pthread_key_t g_key;
static AetherExecContext* port_current(void) {
    return (AetherExecContext*)pthread_getspecific(g_key);
}
static int port_set_current(AetherExecContext* ctx) {
    return pthread_setspecific(g_key, ctx) == 0 ? 0 : -1;
}
static void port_key_ends(void* ctx) {
    aether_context_task_ended((AetherExecContext*)ctx);
}
static void install_port(void) {
    if (pthread_key_create(&g_key, port_key_ends) != 0) abort();
    static const AetherContextPort port = { port_current, port_set_current, NULL };
    if (aether_context_port_install(&port) != 0) {
        fprintf(stderr, "aether_context_port_install refused\n");
        exit(2);
    }
}
#  define ATTACH()                                               \
      do {                                                       \
          if (aether_context_attach() != 0) {                    \
              fprintf(stderr, "aether_context_attach failed\n"); \
              exit(2);                                           \
          }                                                      \
      } while (0)
#  define DETACH() (void)aether_context_detach()
#else
static void install_port(void) {}
#  define ATTACH() ((void)0)
#  define DETACH() ((void)0)
#endif

static int mode_all = 1, mode_deadline = 0, mode_caller = 0;

static int identity_is(const char* want) {
    const void* s = aether_who();
    int ok = aether_last_outcome() == AETHER_OK && s && strcmp(aether_string_data(s), want) == 0;
    if (s) aether_ctxlib_string_release(s);
    return ok;
}

static void* interrupted_task(void* arg) {
    (void)arg;
    ATTACH();
    AetherExecLimits lim = { 64 * 1024, 0, AETHER_DEADLINE_ELAPSED, NULL };
    AetherExecAccount* acct = aether_exec_account_new(&lim);
    if (!acct || aether_exec_use(acct, NULL) != 0) {
        fprintf(stderr, "I: no account\n");
        exit(2);
    }
    (void)aether_set_caller("I", NULL, NULL, 0, 0);
    for (int r = 0; r < ROUNDS; r++) {
        int v, o;
        /* The worker has made a round more since the last: the two overlap
         * all along, however the scheduler starts them. */
        while (atomic_load(&g_w_rounds) <= r) sched_yield();
        if (mode_all) {
            /* Past its allowance: 100,000 * 16 bytes. */
            v = aether_grow(100000);
            o = aether_last_outcome();
            if (o != AETHER_MEMORY_CAP || v != 0) { atomic_fetch_add(&g_i_bad, 1); note_bad("I", "quota", o, AETHER_MEMORY_CAP); }
            if (aether_exec_account_used(acct) != 0) { atomic_fetch_add(&g_i_bad, 1); note_bad("I", "quota leftover", (int)aether_exec_account_used(acct), 0); }
        }
        if (mode_all || mode_deadline) {
            aether_set_call_deadline(5);
            v = aether_spin(2000000000);
            o = aether_last_outcome();
            aether_set_call_deadline(0);
            if (o != AETHER_DEADLINE) { atomic_fetch_add(&g_i_bad, 1); note_bad("I", "deadline", o, AETHER_DEADLINE); }
        }
        if (mode_all) {
            g_cancel_token = aether_exec_token();
            atomic_store(&g_cancel_ready, 1);
            v = aether_spin(2000000000);
            o = aether_last_outcome();
            if (o != AETHER_CANCELLED) { atomic_fetch_add(&g_i_bad, 1); note_bad("I", "cancel", o, AETHER_CANCELLED); }
            v = aether_boom(1);
            o = aether_last_outcome();
            if (o != AETHER_PANICKED || strcmp(aether_last_outcome_text(), "boom") != 0) {
                atomic_fetch_add(&g_i_bad, 1);
                note_bad("I", "panic", o, AETHER_PANICKED);
            }
        }
        if (mode_all || mode_caller) {
            if (!identity_is("I")) { atomic_fetch_add(&g_i_bad, 1); note_bad("I", "identity", 0, 1); }
        }
        (void)v;
    }
    aether_exec_use(NULL, NULL);
    aether_exec_account_free(acct);
    atomic_store(&g_i_done, 1);
    DETACH();
    return NULL;
}

static void* canceller(void* arg) {
    (void)arg;
    /* Touches no Aether state but the cancel cell: no context needed. */
    while (!atomic_load(&g_i_done)) {
        if (atomic_exchange(&g_cancel_ready, 0)) {
            sleep_ms(2);
            (void)aether_exec_cancel(g_cancel_token);
        }
        sleep_ms(1);
    }
    return NULL;
}

static void* worker_task(void* arg) {
    (void)arg;
    ATTACH();
    AetherExecLimits lim = { 0, 0, AETHER_DEADLINE_ELAPSED, NULL };
    AetherExecAccount* acct = aether_exec_account_new(&lim);
    if (!acct || aether_exec_use(acct, NULL) != 0) {
        fprintf(stderr, "W: no account\n");
        exit(2);
    }
    (void)aether_set_caller("W", NULL, NULL, 0, 0);
    while (!atomic_load(&g_i_done)) {
        int v, o;
        if (mode_all) {
            v = aether_grow(64);
            o = aether_last_outcome();
            if (o != AETHER_OK || v != 1024) { atomic_fetch_add(&g_w_bad, 1); note_bad("W", "grow", o, AETHER_OK); }
            if (aether_exec_account_used(acct) != 0) { atomic_fetch_add(&g_w_bad, 1); note_bad("W", "grow leftover", (int)aether_exec_account_used(acct), 0); }
            v = aether_caught(1);
            o = aether_last_outcome();
            if (o != AETHER_OK || v != 7) { atomic_fetch_add(&g_w_bad, 1); note_bad("W", "caught", o, AETHER_OK); }
        }
        if (mode_all || mode_deadline) {
            v = aether_spin(20000);
            o = aether_last_outcome();
            if (o != AETHER_OK || v != 99980000) { atomic_fetch_add(&g_w_bad, 1); note_bad("W", "spin", o, AETHER_OK); }
        }
        if (mode_all || mode_caller) {
            if (!identity_is("W")) { atomic_fetch_add(&g_w_bad, 1); note_bad("W", "identity", 0, 1); }
        }
        atomic_fetch_add(&g_w_rounds, 1);
    }
    if (aether_exec_account_tripped(acct) != 0) { atomic_fetch_add(&g_w_bad, 1); note_bad("W", "tripped", aether_exec_account_tripped(acct), 0); }
    aether_exec_use(NULL, NULL);
    aether_exec_account_free(acct);
    DETACH();
    return NULL;
}

static int run_two(void) {
    pthread_t i, w, c;
    pthread_create(&w, NULL, worker_task, NULL);
    pthread_create(&i, NULL, interrupted_task, NULL);
    pthread_create(&c, NULL, canceller, NULL);
    pthread_join(i, NULL);
    pthread_join(w, NULL);
    pthread_join(c, NULL);
    printf("interrupted task: %d rounds, %d wrong outcomes\n", ROUNDS, atomic_load(&g_i_bad));
    printf("worker task: %s rounds, %d wrong results\n", atomic_load(&g_w_rounds) > 0 ? "some" : "no",
           atomic_load(&g_w_bad));
    if (g_first_bad[0]) fprintf(stderr, "first: %s\n", g_first_bad);
    return atomic_load(&g_i_bad) || atomic_load(&g_w_bad) ? 1 : 0;
}

#ifndef NO_CONTEXT_API
static void* ends_attached(void* arg) {
    (void)arg;
    ATTACH();
    (void)aether_set_caller("gone", NULL, NULL, 0, 0);
    (void)aether_exec_token();
    (void)aether_caught(1);
    return NULL;   /* no detach: the key's destructor ends the context */
}

static int g_churn_bad = 0;

static void* churn_task(void* arg) {
    (void)arg;
    int bad = 0;
    void* first_cell = NULL;
    for (int k = 0; k < 2000; k++) {
        ATTACH();
        AetherExecToken t = aether_exec_token();
        if (k == 0) first_cell = t.cell;
        /* One cell, reused by each context in turn: an old token's serial is
         * below every new one, so it cancels nothing. */
        else if (t.cell != first_cell) bad++;
        (void)aether_set_caller("churn", NULL, NULL, 0, 0);
        if (aether_caught(1) != 7) bad++;
        if (aether_boom(1) != 0 || aether_last_outcome() != AETHER_PANICKED) bad++;
        if (k > 0) {
            /* The previous context's token: spent, it must not cancel this one. */
            static AetherExecToken prev;
            (void)aether_exec_cancel(prev);
            if (aether_spin(1000) != 249000 || aether_last_outcome() != AETHER_OK) bad++;
            prev = aether_exec_token();
        }
        if (aether_context_detach() != 0) bad++;
    }
    /* Detach with none attached, and attach twice: both refused. */
    if (aether_context_detach() != -1) bad++;
    ATTACH();
    if (aether_context_attach() != -1) bad++;
    DETACH();
    g_churn_bad = bad;
    return NULL;
}

static int churn(void) {
    int bad = 0;
    /* The installing thread keeps the built-in context: it cannot attach
     * another or detach that one. */
    if (aether_context_attach() != -1) bad++;
    if (aether_context_detach() != -1) bad++;
    pthread_t t;
    pthread_create(&t, NULL, churn_task, NULL);
    pthread_join(t, NULL);
    bad += g_churn_bad;
    for (int k = 0; k < 200; k++) {
        pthread_create(&t, NULL, ends_attached, NULL);
        pthread_join(t, NULL);
    }
    printf("churn: 2000 attach/detach rounds, 200 threads ended attached, %d wrong\n", bad);
    return bad ? 1 : 0;
}

static void* asks_off_the_actors_task(void* arg) {
    (void)arg;
    ATTACH();
    (void)aether_ask_echo(1);   /* must not return */
    printf("a task that does not own the actors ran one\n");
    return NULL;
}

static void* enters_unattached(void* arg) {
    (void)arg;
    (void)aether_caught(1);   /* must not return */
    printf("an unattached thread ran Aether code\n");
    return NULL;
}
#endif

#if defined(NET_MODES) && !defined(NO_CONTEXT_API)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

extern void     http_clock_pin(void);
extern void     http_clock_unpin(void);
extern uint64_t http_clock_ms(void);
extern void*    http_get_raw(const char* url);
extern int      http_response_status(void* r);
extern int      http_response_body_length(void* r);
extern const char* http_response_header_raw(void* r, const char* name);
extern void     http_response_free(void* r);
extern int      http_client_pool_idle_count_raw(void);
extern void*    list_new(void);
extern void     list_free(void* list);
extern void     aether_sandbox_push(void* grants);
extern void     aether_sandbox_pop(void);
extern int      aether_sandbox_depth(void);
extern int      aether_sandbox_check_slow(const char* category, const char* resource);

/* Two tasks, one holding something of its own while the other looks. */
static _Atomic int g_held = 0, g_looked = 0;
static int g_net_bad = 0;
static char g_net_first[256];
static void net_bad(const char* what) {
    if (!g_net_first[0]) snprintf(g_net_first, sizeof g_net_first, "%s", what);
    g_net_bad++;
}

static void* clock_holder(void* arg) {
    (void)arg;
    ATTACH();
    http_clock_pin();
    uint64_t pinned = http_clock_ms();
    atomic_store(&g_held, 1);
    while (!atomic_load(&g_looked)) sleep_ms(1);
    /* Its own pin held while the other read the clock. */
    if (http_clock_ms() != pinned) net_bad("the holder's pin moved");
    http_clock_unpin();
    DETACH();
    return NULL;
}

static void* clock_reader(void* arg) {
    (void)arg;
    ATTACH();
    while (!atomic_load(&g_held)) sleep_ms(1);
    uint64_t a = http_clock_ms();
    sleep_ms(40);
    uint64_t b = http_clock_ms();
    if (b < a + 20) net_bad("the reader read the other task's pinned clock");
    atomic_store(&g_looked, 1);
    DETACH();
    return NULL;
}

static void* sandbox_holder(void* arg) {
    (void)arg;
    ATTACH();
    void* nothing = list_new();         /* an empty grant list: nothing allowed */
    aether_sandbox_push(nothing);
    if (aether_sandbox_depth() != 1) net_bad("the holder's level is not in force");
    if (aether_sandbox_check_slow("env", "HOME") != 0) net_bad("the holder's level refused nothing");
    atomic_store(&g_held, 1);
    while (!atomic_load(&g_looked)) sleep_ms(1);
    if (aether_sandbox_depth() != 1) net_bad("the holder's level went");
    aether_sandbox_pop();
    list_free(nothing);
    DETACH();
    return NULL;
}

static void* sandbox_other(void* arg) {
    (void)arg;
    ATTACH();
    while (!atomic_load(&g_held)) sleep_ms(1);
    if (aether_sandbox_depth() != 0) net_bad("the other task saw the holder's sandbox level");
    if (aether_sandbox_check_slow("env", "HOME") != 1) net_bad("the other task was refused by the holder's level");
    atomic_store(&g_looked, 1);
    DETACH();
    return NULL;
}

static int run_pair(void* (*holder)(void*), void* (*other)(void*), const char* what) {
    pthread_t a, b;
    pthread_create(&a, NULL, holder, NULL);
    pthread_create(&b, NULL, other, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("%s: %d wrong\n", what, g_net_bad);
    if (g_net_first[0]) fprintf(stderr, "first: %s\n", g_net_first);
    return g_net_bad ? 1 : 0;
}

/* The local server: keep-alive, each response echoing the request's path. */
static int g_port;

static void* serve_conn(void* arg) {
    int fd = (int)(intptr_t)arg;
    char req[2048];
    size_t have = 0;
    for (;;) {
        char* end;
        while (!(end = (have ? strstr(req, "\r\n\r\n") : NULL))) {
            if (have >= sizeof req - 1) { close(fd); return NULL; }
            ssize_t n = recv(fd, req + have, sizeof req - 1 - have, 0);
            if (n <= 0) { close(fd); return NULL; }
            have += (size_t)n;
            req[have] = '\0';
        }
        char path[256] = "?";
        sscanf(req, "GET %255s", path);
        char out[512];
        int n = snprintf(out, sizeof out,
                         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Pair: a\r\nX-Pair: b\r\n"
                         "X-Path: %s\r\n\r\nok", path);
        if (send(fd, out, (size_t)n, 0) != n) { close(fd); return NULL; }
        size_t used = (size_t)(end + 4 - req);
        memmove(req, req + used, have - used);
        have -= used;
        req[have] = '\0';
    }
}

static void* serve(void* arg) {
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        pthread_t t;
        pthread_create(&t, NULL, serve_conn, (void*)(intptr_t)fd);
        pthread_detach(t);
    }
    return NULL;
}

static int start_server(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (lfd < 0 || bind(lfd, (struct sockaddr*)&a, sizeof a) != 0 || listen(lfd, 64) != 0 ||
        getsockname(lfd, (struct sockaddr*)&a, &len) != 0)
        return -1;
    g_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, serve, (void*)(intptr_t)lfd);
    pthread_detach(t);
    return 0;
}

#define POOL_ROUNDS 300
static _Atomic int g_pool_bad = 0;

static void* pool_task(void* arg) {
    const char* who = (const char*)arg;
    ATTACH();
    for (int i = 0; i < POOL_ROUNDS; i++) {
        char url[128], path[64];
        snprintf(path, sizeof path, "/%s/%d", who, i);
        snprintf(url, sizeof url, "http://127.0.0.1:%d%s", g_port, path);
        void* r = http_get_raw(url);
        if (!r || http_response_status(r) != 200 || http_response_body_length(r) != 2 ||
            strcmp(http_response_header_raw(r, "X-Path"), path) != 0 ||
            strcmp(http_response_header_raw(r, "X-Pair"), "a, b") != 0) {
            if (atomic_fetch_add(&g_pool_bad, 1) == 0)
                fprintf(stderr, "first: %s round %d: status %d, X-Path \"%s\"\n", who, i,
                        r ? http_response_status(r) : -1, r ? http_response_header_raw(r, "X-Path") : "");
        }
        if (r) http_response_free(r);
    }
    DETACH();
    return NULL;
}

/* std.tcp's deadline and cancel (aether_net.h). */
typedef struct { void* _0; const char* _1; } HostOpenResult;
extern HostOpenResult tcp_connect_until_raw(const char* host, int port, long long due_ms, void* cancel);
extern HostOpenResult tcp_cancel_new_raw(void);
extern int      tcp_cancel_raw(void* cancel);
extern void     tcp_cancel_free_raw(void* cancel);
extern int      tcp_receive_into_raw(void* sock, void* buf, int cap);
extern int      tcp_stop_raw(void* sock);
extern int      tcp_close(void* sock);
extern int64_t  os_now_monotonic_ms_raw(void);   /* the clock due_ms is on */
#define HOST_TCP_RECV_DEADLINE  (-3)
#define HOST_TCP_RECV_CANCELLED (-4)

static int g_silent_port;
static void* silent_serve(void* arg) {
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);   /* held open, never written to */
        (void)fd;
    }
    return NULL;
}

static void* g_tcp_cancel;
static _Atomic int g_tcp_reading = 0;

static void* native_canceller(void* arg) {
    (void)arg;
    /* Never attaches: a cancel touches only the handle. */
    while (!atomic_load(&g_tcp_reading)) sleep_ms(1);
    sleep_ms(100);
    (void)tcp_cancel_raw(g_tcp_cancel);
    return NULL;
}

static long long now_ms(void) { return (long long)os_now_monotonic_ms_raw(); }

static void* tcp_reader(void* arg) {
    (void)arg;
    char buf[16];
    ATTACH();
    HostOpenResult r = tcp_connect_until_raw("127.0.0.1", g_silent_port, now_ms() + 20000, g_tcp_cancel);
    if (!r._0) { net_bad("connect_until under a cancel handle failed"); DETACH(); return NULL; }
    atomic_store(&g_tcp_reading, 1);
    long long t0 = now_ms();
    int n = tcp_receive_into_raw(r._0, buf, sizeof buf);
    if (n != HOST_TCP_RECV_CANCELLED || tcp_stop_raw(r._0) != 2) net_bad("the native task's cancel did not stop the read");
    if (now_ms() - t0 > 10000) net_bad("the cancel came late");
    tcp_close(r._0);
    r = tcp_connect_until_raw("127.0.0.1", g_silent_port, now_ms() + 300, NULL);
    if (!r._0) { net_bad("connect_until under a deadline failed"); DETACH(); return NULL; }
    t0 = now_ms();
    n = tcp_receive_into_raw(r._0, buf, sizeof buf);
    long long took = now_ms() - t0;
    if (n != HOST_TCP_RECV_DEADLINE || tcp_stop_raw(r._0) != 1) net_bad("the deadline did not stop the read");
    if (took > 5000) net_bad("the deadline came late");
    tcp_close(r._0);
    DETACH();
    return NULL;
}

static int tcpstop(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (lfd < 0 || bind(lfd, (struct sockaddr*)&a, sizeof a) != 0 || listen(lfd, 8) != 0 ||
        getsockname(lfd, (struct sockaddr*)&a, &len) != 0) {
        fprintf(stderr, "no local listener\n");
        return 2;
    }
    g_silent_port = ntohs(a.sin_port);
    pthread_t l, c, t;
    pthread_create(&l, NULL, silent_serve, (void*)(intptr_t)lfd);
    pthread_detach(l);
    HostOpenResult h = tcp_cancel_new_raw();
    if (!h._0) { fprintf(stderr, "no cancel handle: %s\n", h._1); return 2; }
    g_tcp_cancel = h._0;
    pthread_create(&c, NULL, native_canceller, NULL);
    pthread_create(&t, NULL, tcp_reader, NULL);
    pthread_join(t, NULL);
    pthread_join(c, NULL);
    tcp_cancel_free_raw(g_tcp_cancel);
    printf("tcpstop: %d wrong\n", g_net_bad);
    if (g_net_first[0]) fprintf(stderr, "first: %s\n", g_net_first);
    return g_net_bad ? 1 : 0;
}

static int pool(void) {
    if (start_server() != 0) { fprintf(stderr, "no local server\n"); return 2; }
    pthread_t a, b;
    pthread_create(&a, NULL, pool_task, (void*)"A");
    pthread_create(&b, NULL, pool_task, (void*)"B");
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    int idle = http_client_pool_idle_count_raw();
    printf("pool: %d requests from two tasks, %d wrong, %s idle\n", 2 * POOL_ROUNDS,
           atomic_load(&g_pool_bad), idle >= 1 && idle <= 8 ? "some" : "a wrong count of");
    return atomic_load(&g_pool_bad) ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
    const char* m = argc > 1 ? argv[1] : "all";
    setvbuf(stdout, NULL, _IONBF, 0);
    install_port();
    if (strcmp(m, "deadline") == 0) { mode_all = 0; mode_deadline = 1; }
    else if (strcmp(m, "caller") == 0) { mode_all = 0; mode_caller = 1; }
#if defined(NET_MODES) && !defined(NO_CONTEXT_API)
    else if (strcmp(m, "clock") == 0) return run_pair(clock_holder, clock_reader, "clock");
    else if (strcmp(m, "sandbox") == 0) return run_pair(sandbox_holder, sandbox_other, "sandbox");
    else if (strcmp(m, "pool") == 0) return pool();
    else if (strcmp(m, "tcpstop") == 0) return tcpstop();
#endif
#ifndef NO_CONTEXT_API
    else if (strcmp(m, "churn") == 0) return churn();
    else if (strcmp(m, "actors") == 0) {
        /* The installing thread owns the actors: its ask is answered. */
        printf("owner's ask: %d\n", aether_ask_echo(41));
        pthread_t t;
        pthread_create(&t, NULL, asks_off_the_actors_task, NULL);
        pthread_join(t, NULL);
        return 0;
    }
    else if (strcmp(m, "unattached") == 0) {
        pthread_t t;
        pthread_create(&t, NULL, enters_unattached, NULL);
        pthread_join(t, NULL);
        return 0;
    }
#endif
    return run_two();
}
