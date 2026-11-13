/* A fatal error while another thread holds stdout (#3435). The program
 * prints two lines (stdout is a file, so they wait in its buffer), a second
 * thread takes stdout's lock, and the program fails a bounds check, which
 * flushes stdout and aborts.
 *
 *   held <ms>   the thread lets go after <ms> milliseconds, as one in the
 *               middle of a println does: the flush waits for it, and both
 *               lines reach the file (a single try of the lock lost them);
 *   held -1     the thread never lets go, as one blocked writing to a pipe
 *               nobody reads: the flush gives up, and the abort comes at
 *               once (a plain fflush waited for ever). */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "aether_bounds_check.h"

static int g_hold_ms;
/* Set by the holder once it has stdout, read by main until then: atomic,
 * as a flag between two threads must be (a volatile int raced, #3542). */
static atomic_int g_held;

static void* holder(void* arg) {
    (void)arg;
    flockfile(stdout);
    atomic_store(&g_held, 1);
    if (g_hold_ms < 0)
        for (;;) pause();
    usleep((useconds_t)g_hold_ms * 1000);
    funlockfile(stdout);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    g_hold_ms = atoi(argv[1]);
    printf("first line\nsecond line\n");
    pthread_t t;
    if (pthread_create(&t, NULL, holder, NULL) != 0) return 2;
    while (!atomic_load(&g_held)) usleep(1000);
    aether_check_array_access(5, 3, "xs", "held.c", 1);
    return 0;
}
