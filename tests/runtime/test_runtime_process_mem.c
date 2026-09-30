// Process memory readings (#2310): runtime/aether_process_mem.c, held to
// numbers without a compiler in the loop. The Aether surface end to end
// (std.mem heap_in_use, std.os memory_resident / memory_private) is
// tests/regression/test_issue2310_heap_in_use.ae.
#include "test_harness.h"
#include "../../runtime/aether_process_mem.h"
#include <stdlib.h>
#include <string.h>

/* Where each block escapes to: a compiler may drop a malloc/free pair whose
 * pointer goes nowhere, and then there is nothing to measure. */
static void* volatile g_sink;

/* A block held shows in heap_in_use, and freeing it gives it back. 1 MiB is
 * past every allocator's small-block cache and (on glibc) its mmap
 * threshold, so both the arena and the mapped-chunk paths are exercised
 * across the platforms this runs on. */
TEST_CATEGORY(heap_in_use_sees_a_block_and_its_free, TEST_CATEGORY_RUNTIME) {
    int64_t before = aether_heap_in_use();
    if (before < 0) return;   /* an allocator with no statistics (musl) */
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
