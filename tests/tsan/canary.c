/* The canary of `make test-tsan` (#3542, tests/scripts/sanitizer_sweep.sh).
 *
 * A data race every ThreadSanitizer build reports: a thread stores to a
 * plain int while the thread that started it stores to the same int, with
 * nothing ordering the two stores. pthread_create orders what the starting
 * thread did before it, not after, so whichever store comes first, the
 * second races with it; no timing makes the report come or go. The join
 * then orders the read that returns the value. A configuration whose flags
 * never reached the programs `ae build` makes would let it run clean, and
 * the run fails on that instead of reporting a corpus it never checked. */
#include <pthread.h>

static int tsan_canary_cell;

static void* tsan_canary_writer(void* arg) {
    tsan_canary_cell = *(const int*)arg;
    return NULL;
}

int tsan_canary_race(int value) {
    pthread_t t;
    if (pthread_create(&t, NULL, tsan_canary_writer, &value) != 0) return -1;
    tsan_canary_cell = value + 1;
    pthread_join(t, NULL);
    return tsan_canary_cell;
}
