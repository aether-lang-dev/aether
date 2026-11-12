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
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/* Where each block escapes to: a compiler may drop a malloc/free pair whose
 * pointer goes nowhere, and then there is nothing to measure. */
static void* volatile g_sink;

/* A block held shows in heap_in_use, and freeing it gives it back. 1 MiB is
 * past every allocator's small-block cache and (on glibc) its mmap
 * threshold, so both the arena and the mapped-chunk paths are exercised
 * across the platforms this runs on. */
/* This is the binary's first test, run while the Windows loader's workers
 * can still be making their first run, each allocating about 1 KB (a free
 * then shows as less than the block): a try during which the thread epoch
 * moved is made again, a bounded number of times. */
TEST_CATEGORY(heap_in_use_sees_a_block_and_its_free, TEST_CATEGORY_RUNTIME) {
    if (aether_heap_in_use() < 0) return;   /* no statistics: musl, or valgrind serving malloc */
    size_t n = 1u << 20;
    int64_t before = 0, held = 0, after = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        int64_t e0 = aether_thread_epoch();
        before = aether_heap_in_use();
        char* p = (char*)malloc(n);
        ASSERT_NOT_NULL(p);
        g_sink = p;
        memset(p, 1, n);
        held = aether_heap_in_use();
        free(p);
        after = aether_heap_in_use();
        if (aether_thread_epoch() == e0) break;
        sleep_ms(5);
    }
    ASSERT_TRUE(held - before >= (int64_t)n);
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
/* Flags between the test and its waiter thread: atomic (volatile ints raced,
 * #3542). */
static atomic_int g_release_waiter = 0;
static atomic_int g_waiter_running = 0;

/* A thread's start is reported on the thread itself, before its start
 * routine runs (on Windows, the loader's DLL_THREAD_ATTACH), so the
 * routine says it runs and the reader waits for that. */
static void* epoch_waiter(void* arg) {
    (void)arg;
    atomic_store(&g_waiter_running, 1);
    while (!atomic_load(&g_release_waiter)) sched_yield();
    return NULL;
}

TEST_CATEGORY(thread_epoch_moves_when_a_thread_starts_and_ends, TEST_CATEGORY_RUNTIME) {
    int64_t before = aether_thread_epoch();
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
    ASSERT_TRUE(before >= 0);
#endif
    if (before < 0) return;
    /* On macOS the epoch is the thread count, and a thread an earlier test
     * joined can still be listed while its Mach thread finishes
     * terminating. If it drops out after `before` is read, the count falls
     * by one as the waiter's start raises it by one, and the two cancel:
     * running == before with a thread started. So `before` is taken only
     * once the epoch has held still, 30 reads 1 ms apart, in a bounded
     * wait. */
    {
        int stable = 0;
        for (int i = 0; stable < 30 && i < 5000; i++) {
            sleep_ms(1);
            int64_t now = aether_thread_epoch();
            if (now == before) stable++;
            else { before = now; stable = 0; }
        }
        ASSERT_TRUE(stable >= 30);
    }
    atomic_store(&g_release_waiter, 0);
    atomic_store(&g_waiter_running, 0);
    pthread_t t;
    ASSERT_TRUE(pthread_create(&t, NULL, epoch_waiter, NULL) == 0);
    while (!atomic_load(&g_waiter_running)) sched_yield();
    /* The start is counted by the time the thread runs on most platforms,
     * but wait for it a bounded while, as the end is waited for below. */
    int64_t running = aether_thread_epoch();
    for (int i = 0; running == before && i < 2000; i++) {
        sleep_ms(1);
        running = aether_thread_epoch();
    }
    ASSERT_TRUE(running != before);
    atomic_store(&g_release_waiter, 1);
    pthread_join(t, NULL);
    /* The end can be reported after the join returns: on macOS the Mach
     * thread finishes terminating on its own, and task_threads lists it
     * until then. Wait for it, a bounded while. */
    int64_t ended = aether_thread_epoch();
    for (int i = 0; ended == running && i < 2000; i++) {
        sleep_ms(1);
        ended = aether_thread_epoch();
    }
    ASSERT_TRUE(ended != running);
}

#ifdef _WIN32
/* The parallel loader's workers are started with thread notifications off,
 * so no DLL_THREAD_ATTACH reports them, and their first run allocates: the
 * epoch moves with the CPU time of threads the loader has not reported.
 * Such a thread is made here the way the loader makes its own
 * (NtCreateThreadEx, THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH), and the epoch
 * stays a non-negative number through every start and end (it is -1 only
 * where there is no answer). */
typedef LONG (NTAPI *nt_create_thread_ex_fn)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID,
                                             ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
static HANDLE g_unreported_go = NULL;
static volatile LONG g_unreported_done = 0;
static DWORD WINAPI unreported_body(LPVOID arg) {
    (void)arg;
    WaitForSingleObject(g_unreported_go, INFINITE);
    volatile unsigned long spin = 0;
    for (unsigned long i = 0; i < 20000000ul; i++) spin += i;
    InterlockedExchange(&g_unreported_done, 1);
    return 0;
}

TEST_CATEGORY(thread_epoch_moves_when_an_unreported_thread_runs, TEST_CATEGORY_RUNTIME) {
    for (int i = 0; i < 8; i++) {
        pthread_t t;
        atomic_store(&g_release_waiter, 1);
        atomic_store(&g_waiter_running, 0);
        ASSERT_TRUE(pthread_create(&t, NULL, epoch_waiter, NULL) == 0);
        pthread_join(t, NULL);
        ASSERT_TRUE(aether_thread_epoch() >= 0);
    }
    nt_create_thread_ex_fn create = (nt_create_thread_ex_fn)(void*)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx");
    /* Without NtGetNextThread (an old Wine) the runtime cannot walk them. */
    if (!create || !GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread")) return;
    HANDLE h = NULL;
    g_unreported_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    ASSERT_TRUE(g_unreported_go != NULL);
    g_unreported_done = 0;
    ASSERT_TRUE(create(&h, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), (PVOID)unreported_body,
                       NULL, 0x2 /* SKIP_THREAD_ATTACH */, 0, 0, 0, NULL) >= 0);
    /* Blocked on the event, it holds still: the epoch does too. */
    /* (The loader's own workers can still be running: wait, a bounded
     * while, for 20 reads 1 ms apart to agree.) */
    int64_t before = aether_thread_epoch();
    ASSERT_TRUE(before >= 0);
    int stable = 0;
    for (int i = 0; stable < 20 && i < 3000; i++) {
        sleep_ms(1);
        int64_t now = aether_thread_epoch();
        if (now == before) stable++;
        else { before = now; stable = 0; }
    }
    ASSERT_TRUE(stable >= 20);
    SetEvent(g_unreported_go);
    while (!g_unreported_done) sleep_ms(1);
    int64_t after = aether_thread_epoch();
    ASSERT_TRUE(after >= 0);
    ASSERT_TRUE(after != before);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    CloseHandle(g_unreported_go);
}
#endif
