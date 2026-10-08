// Process memory readings (#2310): runtime/aether_process_mem.c, held to
// numbers without a compiler in the loop. The Aether surface end to end
// (std.mem heap_in_use, std.os memory_resident / memory_private) is
// tests/regression/test_issue2310_heap_in_use.ae.
#include "test_harness.h"
#include "../../runtime/aether_process_mem.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

/* Where each block escapes to: a compiler may drop a malloc/free pair whose
 * pointer goes nowhere, and then there is nothing to measure. */
static void* volatile g_sink;

/* A block held shows in heap_in_use, and freeing it gives it back. 1 MiB is
 * past every allocator's small-block cache and (on glibc) its mmap
 * threshold, so both the arena and the mapped-chunk paths are exercised
 * across the platforms this runs on. */
TEST_CATEGORY(heap_in_use_sees_a_block_and_its_free, TEST_CATEGORY_RUNTIME) {
    int64_t before = aether_heap_in_use();
    if (before < 0) return;   /* no statistics: musl, or valgrind serving malloc */
    size_t n = 1u << 20;
    char* p = (char*)malloc(n);
    ASSERT_NOT_NULL(p);
    g_sink = p;
    memset(p, 1, n);
    int64_t held = aether_heap_in_use();
    ASSERT_TRUE(held - before >= (int64_t)n);
    free(p);
    int64_t after = aether_heap_in_use();
    ASSERT_TRUE(held - after >= (int64_t)n);
}

/* Many small blocks, the shape a per-frame leak has: they add up. */
TEST_CATEGORY(heap_in_use_sees_small_blocks, TEST_CATEGORY_RUNTIME) {
    if (aether_heap_in_use() < 0) return;
    enum { COUNT = 2000, SIZE = 64 };
    void** keep = (void**)malloc(sizeof(void*) * COUNT);
    ASSERT_NOT_NULL(keep);
    int64_t before = aether_heap_in_use();
    for (int i = 0; i < COUNT; i++) g_sink = keep[i] = malloc(SIZE);
    int64_t held = aether_heap_in_use();
    /* At least half: an allocator may hand back blocks it had cached and
     * already counted. */
    ASSERT_TRUE(held - before >= (int64_t)COUNT * SIZE / 2);
    for (int i = 0; i < COUNT; i++) free(keep[i]);
    free(keep);
}

TEST_CATEGORY(process_memory_is_reported_or_unsupported, TEST_CATEGORY_RUNTIME) {
    int64_t resident = aether_process_resident();
    int64_t priv = aether_process_private();
    ASSERT_TRUE(resident == -1 || resident > 0);
    ASSERT_TRUE(priv == -1 || priv > 0);
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    ASSERT_TRUE(resident > 0);
    ASSERT_TRUE(priv > 0);
#endif
}

/* aether_thread_epoch changes when a thread starts and again when it ends
 * (#2551): a thread holds heap blocks of its own while it lives, so a heap
 * count is compared only between reads with the same epoch. */
static volatile int g_release_waiter = 0;
static volatile int g_waiter_running = 0;

/* A thread's start is reported on the thread itself, before its start
 * routine runs (on Windows, the loader's DLL_THREAD_ATTACH), so the
 * routine says it runs and the reader waits for that. */
static void* epoch_waiter(void* arg) {
    (void)arg;
    g_waiter_running = 1;
    while (!g_release_waiter) sched_yield();
    return NULL;
}

TEST_CATEGORY(thread_epoch_moves_when_a_thread_starts_and_ends, TEST_CATEGORY_RUNTIME) {
    int64_t before = aether_thread_epoch();
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
    ASSERT_TRUE(before >= 0);
#endif
    if (before < 0) return;
    ASSERT_TRUE(aether_thread_epoch() == before);
    g_release_waiter = 0;
    g_waiter_running = 0;
    pthread_t t;
    ASSERT_TRUE(pthread_create(&t, NULL, epoch_waiter, NULL) == 0);
    while (!g_waiter_running) sched_yield();
    int64_t running = aether_thread_epoch();
    ASSERT_TRUE(running != before);
    g_release_waiter = 1;
    pthread_join(t, NULL);
    int64_t ended = aether_thread_epoch();
    ASSERT_TRUE(ended != running);
}
