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
 */
#include <pthread.h>
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

int main(int argc, char** argv) {
    const char* m = argc > 1 ? argv[1] : "all";
    setvbuf(stdout, NULL, _IONBF, 0);
    install_port();
    if (strcmp(m, "deadline") == 0) { mode_all = 0; mode_deadline = 1; }
    else if (strcmp(m, "caller") == 0) { mode_all = 0; mode_caller = 1; }
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
