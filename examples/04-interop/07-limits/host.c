/* The C program of examples/04-interop/07-limits: it runs script.ae's
 * exports under a deadline, a cancel from another thread and an execution
 * account, as run.sh links them. */
#include <libaether.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

int32_t aether_spin(void);
int32_t aether_grow(int32_t n);

static const char* outcome_name(int outcome) {
    switch (outcome) {
    case AETHER_OK: return "ok";
    case AETHER_PANICKED: return "panicked";
    case AETHER_DEADLINE: return "deadline";
    case AETHER_CANCELLED: return "cancelled";
    case AETHER_MEMORY_CAP: return "memory cap";
    case AETHER_OUT_OF_MEMORY: return "out of memory";
    default: return "another failure";
    }
}

static void report(const char* what, int32_t result) {
    printf("%s: returned %d, %s\n", what, (int)result, outcome_name(aether_last_outcome()));
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = { 0, (long)ms * 1000000L };
    nanosleep(&t, NULL);
#endif
}

/* Cancels the call the token names, once it has run a little. */
static void* cancel_soon(void* arg) {
    sleep_ms(20);
    aether_exec_cancel(*(AetherExecToken*)arg);
    return NULL;
}

int main(void) {
    /* A deadline for every call this thread makes, until it is set to 0. */
    aether_set_call_deadline(50);
    report("spin under a 50 ms deadline", aether_spin());
    aether_set_call_deadline(0);

    /* A cancel: the token names the next call this thread makes. */
    AetherExecToken token = aether_exec_token();
    pthread_t canceller;
    pthread_create(&canceller, NULL, cancel_soon, &token);
    report("spin cancelled from another thread", aether_spin());
    pthread_join(canceller, NULL);

    /* A budget: 64 KiB of live memory for everything the script holds. */
    AetherExecLimits limits = { 64 * 1024, 0, AETHER_DEADLINE_ELAPSED, NULL };
    AetherExecAccount* script = aether_exec_account_new(&limits);
    AetherExecAccount* host_own = NULL;
    aether_exec_use(script, &host_own); /* this thread's calls charge `script` */
    report("grow(100) under a 64 KiB budget", aether_grow(100));
    report("grow(100000) under the same budget", aether_grow(100000));
    int tripped = aether_exec_account_tripped(script) & AETHER_ACCOUNT_TRIPPED_MEMORY;
    printf("the budget was exceeded: %s\n", tripped ? "yes" : "no");
    report("grow(10) after that", aether_grow(10));
    aether_exec_use(host_own, NULL); /* back to the host's own */
    aether_exec_account_free(script);
    return 0;
}
