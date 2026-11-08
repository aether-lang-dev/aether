/**
 * Execution accounts (#2954) after the review of their fixes: what a free,
 * a nesting and a parent do to them.
 *
 * - #3407: freeing an account leaves nothing naming it: not the thread's
 *   account in use, not an execution that began under it.
 * - #3408: freeing an account credits its ancestors with what its live
 *   blocks were charged, and its children keep narrowing under its parent.
 * - #3409: an ACTIVE account is charged once for time spent in executions
 *   under it, however they nest, and however many are timed at once;
 *   an account that cannot be timed for want of memory ends its call.
 * - #3410: freeing an account is linear in its blocks.
 * - #3412: an account made under a parent that is not live is refused.
 */

#include "test_harness.h"
#include "../../runtime/aether_exec.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/actors/aether_panic.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define ea_sleep_ms(ms) Sleep(ms)
/* #3488: a free's cost is CPU work, so it is timed by the thread's own CPU
 * clock where there is one fine enough: a wall clock counts the time the
 * thread was not running, and on a loaded runner (macOS ASan) one 40,000-
 * block free took 22 times one of 10,000. Windows' thread times tick at
 * 15.6 ms, too coarse for a free of a few milliseconds: there it is the
 * performance counter, as before. */
static double ea_cpu_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <unistd.h>
#include <time.h>
#define ea_sleep_ms(ms) usleep((ms) * 1000)
static double ea_cpu_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
#endif

static AetherExecAccount* ea_new(uint64_t bytes, int64_t ms, int counts, AetherExecAccount* parent) {
    AetherExecLimits l = { bytes, ms, counts, parent };
    return aether_exec_account_new(&l);
}

/* #3407: an account put in use, then freed, is in use no more. */
TEST_CATEGORY(accounts_free_ends_its_use, TEST_CATEGORY_RUNTIME) {
    AetherExecAccount* a = ea_new(1 << 20, 0, AETHER_DEADLINE_ELAPSED, NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(0, aether_exec_use(a, NULL));
    aether_exec_account_free(a);
    AetherExecAccount* here = aether_account_here();
    void* p = aether_caps_malloc(80);
    aether_caps_free(p, 80);
    aether_exec_use(NULL, NULL);
    ASSERT_NULL(here);
}

/* #3407: an execution that began under an account the code it runs frees
 * gives nothing back to it: after it, no account is in use. */
static AetherExecAccount* g_ea_freed;

static void ea_run_freeing(int switch_first) {
    AetherJmpFrame* x = aether_exec_enter();
    if (AETHER_SIGSETJMP(x->buf, 1)) {
        aether_exec_failed();
        return;
    }
    if (switch_first) aether_exec_use(NULL, NULL);
    aether_exec_account_free(g_ea_freed);
    void* p = aether_caps_malloc(64);
    aether_caps_free(p, 64);
    aether_exec_leave();
}

TEST_CATEGORY(accounts_free_inside_an_execution_under_it, TEST_CATEGORY_RUNTIME) {
    AetherExecAccount* in_use_after[2];
    int outcome[2];
    for (int k = 0; k < 2; k++) {
        g_ea_freed = ea_new(1 << 20, 0, AETHER_DEADLINE_ELAPSED, NULL);
        ASSERT_NOT_NULL(g_ea_freed);
        ASSERT_EQ(0, aether_exec_use(g_ea_freed, NULL));
        ea_run_freeing(k);
        outcome[k] = aether_last_outcome();
        in_use_after[k] = aether_account_here();
        aether_exec_use(NULL, NULL);
    }
    ASSERT_EQ(AETHER_OK, outcome[0]);
    ASSERT_EQ(AETHER_OK, outcome[1]);
    ASSERT_NULL(in_use_after[0]);   // freed while in use
    ASSERT_NULL(in_use_after[1]);   // freed after the code put it out of use
}

/* #3408: a child freed while its blocks live credits its parent with them;
 * a parent freed under its children leaves them under its own parent. */
TEST_CATEGORY(accounts_free_credits_the_ancestors, TEST_CATEGORY_RUNTIME) {
    AetherExecAccount* p = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, NULL);
    AetherExecAccount* c = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, p);
    ASSERT_NOT_NULL(c);
    aether_exec_use(c, NULL);
    void* b = aether_caps_malloc(1000);
    aether_exec_use(NULL, NULL);
    uint64_t parent_before = aether_exec_account_used(p);
    aether_exec_account_free(c);
    uint64_t parent_after_free = aether_exec_account_used(p);
    aether_caps_free(b, 1000);
    uint64_t parent_after_block = aether_exec_account_used(p);

    AetherExecAccount* g = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, NULL);
    AetherExecAccount* m = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, g);
    AetherExecAccount* k = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, m);
    aether_exec_use(k, NULL);
    void* q = aether_caps_malloc(500);
    aether_exec_use(NULL, NULL);
    aether_exec_account_free(m);
    uint64_t grand_after_free = aether_exec_account_used(g);
    aether_caps_free(q, 500);
    uint64_t grand_after_block = aether_exec_account_used(g);
    uint64_t child_after_block = aether_exec_account_used(k);
    aether_exec_account_free(k);
    aether_exec_account_free(g);
    aether_exec_account_free(p);

    ASSERT_EQ(1000, parent_before);
    ASSERT_EQ(0, parent_after_free);
    ASSERT_EQ(0, parent_after_block);
    ASSERT_EQ(500, grand_after_free);
    ASSERT_EQ(0, grand_after_block);
    ASSERT_EQ(0, child_after_block);
}

/* #3409: time spent in an execution nested inside another, both under one
 * ACTIVE account (and under a child of it), is charged to it once: never
 * more than the time the two took, read on the clock the runtime charges
 * by. A sleep may take longer than asked (a loaded macOS runner took a
 * 200 ms one to well past 250), so no bound here leans on what a sleep
 * takes: the account's 300 ms hold the 100 ms nested and whatever it
 * overran, and a further 250 ms pass what is left of them. */
static int ea_nested(AetherExecAccount* inner) {
    AetherJmpFrame* x = aether_exec_enter();
    if (AETHER_SIGSETJMP(x->buf, 1)) {
        aether_exec_failed();
        return aether_last_outcome();
    }
    AetherExecAccount* outer = NULL;
    if (inner) aether_exec_use(inner, &outer);
    AetherJmpFrame* y = aether_exec_enter();
    if (AETHER_SIGSETJMP(y->buf, 1)) {
        aether_exec_failed();
    } else {
        ea_sleep_ms(100);
        aether_exec_leave();
    }
    if (inner) aether_exec_use(outer, NULL);
    aether_exec_leave();
    return aether_last_outcome();
}

static int ea_poll_under(int ms) {
    AetherJmpFrame* x = aether_exec_enter();
    if (AETHER_SIGSETJMP(x->buf, 1)) {
        aether_exec_failed();
        return aether_last_outcome();
    }
    ea_sleep_ms(ms);
    (void)aether_exec_poll();
    aether_exec_leave();
    return aether_last_outcome();
}

TEST_CATEGORY(accounts_active_time_once_when_nested, TEST_CATEGORY_RUNTIME) {
    int first[2], tripped[2], past[2];
    int64_t spent[2], took[2];
    for (int k = 0; k < 2; k++) {
        AetherExecAccount* a = ea_new(0, 300, AETHER_DEADLINE_ACTIVE, NULL);
        AetherExecAccount* child = k ? ea_new(0, 0, AETHER_DEADLINE_ELAPSED, a) : NULL;
        aether_exec_use(a, NULL);
        int64_t t0 = aether_caps_now_ns();
        first[k] = ea_nested(child);
        took[k] = aether_caps_now_ns() - t0;
        spent[k] = atomic_load(&a->active_spent_ns);
        tripped[k] = aether_exec_account_tripped(a);
        past[k] = ea_poll_under(250);   /* at least 100 + 250: past its 300 */
        aether_exec_use(NULL, NULL);
        if (child) aether_exec_account_free(child);
        aether_exec_account_free(a);
    }
    for (int k = 0; k < 2; k++) {
        ASSERT_EQ(AETHER_OK, first[k]);
        ASSERT_TRUE(spent[k] >= 100 * 1000000LL);   /* the nested sleep */
        ASSERT_TRUE(spent[k] <= took[k]);           /* charged once */
        ASSERT_EQ(0, tripped[k]);
        ASSERT_EQ(AETHER_DEADLINE, past[k]);
    }
}


/* #3409: more ACTIVE accounts timed at once on one thread than its list
 * holds inline (and than the 16 it once held), each in use in an execution
 * nested inside the one before: every one is charged its time. */
#define EA_DEEP 20
static AetherExecAccount* g_ea_deep[EA_DEEP];

static void ea_deep(int i) {
    AetherJmpFrame* x = aether_exec_enter();
    if (AETHER_SIGSETJMP(x->buf, 1)) {
        aether_exec_failed();
        return;
    }
    if (i + 1 < EA_DEEP) {
        AetherExecAccount* prev = NULL;
        aether_exec_use(g_ea_deep[i + 1], &prev);
        ea_deep(i + 1);
        aether_exec_use(prev, NULL);
    } else {
        ea_sleep_ms(50);
    }
    aether_exec_leave();
}

TEST_CATEGORY(accounts_active_time_many_nested, TEST_CATEGORY_RUNTIME) {
    for (int i = 0; i < EA_DEEP; i++) g_ea_deep[i] = ea_new(0, 10000, AETHER_DEADLINE_ACTIVE, NULL);
    aether_exec_use(g_ea_deep[0], NULL);
    ea_deep(0);
    int outcome = aether_last_outcome();
    aether_exec_use(NULL, NULL);
    int charged = 0;
    for (int i = 0; i < EA_DEEP; i++) {
        if (atomic_load(&g_ea_deep[i]->active_spent_ns) >= 45 * 1000000LL) charged++;
        aether_exec_account_free(g_ea_deep[i]);
    }
    ASSERT_EQ(AETHER_OK, outcome);
    ASSERT_EQ(EA_DEEP, charged);
}
/* #3410: freeing an account of 4n blocks takes about four times as long as
 * one of n, not sixteen. The best of five runs of each, with another
 * account live, so the table outlives the free. */
static double ea_free_ms(int n) {
    double best = 1e18;
    AetherExecAccount* keep = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, NULL);
    void** blocks = (void**)malloc(sizeof(void*) * (size_t)n);
    for (int run = 0; run < 5; run++) {
        AetherExecAccount* a = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, NULL);
        aether_exec_use(a, NULL);
        for (int i = 0; i < n; i++) blocks[i] = aether_caps_malloc(32);
        aether_exec_use(NULL, NULL);
        double t0 = ea_cpu_ms();
        aether_exec_account_free(a);
        double t = ea_cpu_ms() - t0;
        if (t < best) best = t;
        for (int i = 0; i < n; i++) aether_caps_free(blocks[i], 32);
    }
    aether_exec_account_free(keep);
    free(blocks);
    return best;
}

TEST_CATEGORY(accounts_free_is_linear_in_its_blocks, TEST_CATEGORY_RUNTIME) {
    double small = ea_free_ms(10000);
    double large = ea_free_ms(40000);
    if (small < 0.05) small = 0.05;   /* below the timer's grain both are fast */
    fprintf(stderr, "    account_free: 10000 blocks %.2f ms, 40000 blocks %.2f ms\n", small, large);
    ASSERT_TRUE(large < small * 8.0 || large < 2.0);
}


/* #3409: the growth of a thread's list of timed accounts is a checked
 * allocation. The test hook refuses it for the account of the ninth
 * execution nested (the first the inline entries cannot hold): that call
 * ends at its first safe point with AETHER_OUT_OF_MEMORY, having run none
 * of its code untimed, the calls around it end as they would, and the next
 * call under that account times it. */
#define EA_GROW 10
static AetherExecAccount* g_ea_grow[EA_GROW];
static int g_ea_refuse_at = -1;
static int g_ea_refused_outcome = -1;

static void ea_grow_nest(int i) {
    if (i == g_ea_refuse_at) aether_test_fail_alloc_after(1);
    AetherJmpFrame* x = aether_exec_enter();
    if (AETHER_SIGSETJMP(x->buf, 1)) {
        aether_exec_failed();
        return;
    }
    (void)aether_exec_poll();   /* its first safe point */
    if (i + 1 < EA_GROW) {
        AetherExecAccount* prev = NULL;
        aether_exec_use(g_ea_grow[i + 1], &prev);
        ea_grow_nest(i + 1);
        if (i + 1 == g_ea_refuse_at) g_ea_refused_outcome = aether_last_outcome();
        aether_exec_use(prev, NULL);
    } else {
        ea_sleep_ms(50);
    }
    aether_exec_leave();
}

TEST_CATEGORY(accounts_active_timing_growth_refused, TEST_CATEGORY_RUNTIME) {
    for (int i = 0; i < EA_GROW; i++) g_ea_grow[i] = ea_new(0, 10000, AETHER_DEADLINE_ACTIVE, NULL);
    aether_exec_use(g_ea_grow[0], NULL);
    g_ea_refuse_at = 8;
    ea_grow_nest(0);
    int first = aether_last_outcome();
    int64_t refused_spent = atomic_load(&g_ea_grow[8]->active_spent_ns);
    g_ea_refuse_at = -1;
    ea_grow_nest(0);
    int second = aether_last_outcome();
    int64_t spent_after = atomic_load(&g_ea_grow[8]->active_spent_ns);
    int64_t deepest_after = atomic_load(&g_ea_grow[9]->active_spent_ns);
    aether_exec_use(NULL, NULL);
    aether_test_fail_alloc_after(0);
    for (int i = 0; i < EA_GROW; i++) aether_exec_account_free(g_ea_grow[i]);
    ASSERT_EQ(AETHER_OUT_OF_MEMORY, g_ea_refused_outcome);
    ASSERT_EQ(AETHER_OK, first);
    ASSERT_TRUE(refused_spent < 20 * 1000000LL);
    ASSERT_EQ(AETHER_OK, second);
    ASSERT_TRUE(spent_after >= 45 * 1000000LL);
    ASSERT_TRUE(deepest_after >= 45 * 1000000LL);
}

/* #3412: a parent that is not live (freed, or never made by these
 * accounts) is refused, not dropped: the child would be wider than it. */
TEST_CATEGORY(accounts_unknown_parent_refused, TEST_CATEGORY_RUNTIME) {
    AetherExecAccount* p = ea_new(100, 0, AETHER_DEADLINE_ELAPSED, NULL);
    aether_exec_account_free(p);
    AetherExecAccount* under_freed = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, p);
    static char not_an_account[256];
    AetherExecAccount* under_foreign = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, (AetherExecAccount*)not_an_account);
    if (under_freed) aether_exec_account_free(under_freed);
    if (under_foreign) aether_exec_account_free(under_foreign);
    ASSERT_NULL(under_freed);
    ASSERT_NULL(under_foreign);
}

/* A realloc to no bytes is a free: the account the block was charged to is
 * credited with it. It read as a failed realloc, which charged the account
 * for the freed block again (gcc 12's -Wuse-after-free at the realloc_end
 * call, seen first on NetBSD). */
TEST_CATEGORY(accounts_realloc_to_zero_is_a_free, TEST_CATEGORY_RUNTIME) {
    AetherExecAccount* a = ea_new(0, 0, AETHER_DEADLINE_ELAPSED, NULL);
    ASSERT_NOT_NULL(a);
    aether_exec_use(a, NULL);
    void* b = aether_caps_malloc(1000);
    uint64_t charged = aether_exec_account_used(a);
    void* r = aether_caps_realloc(b, 1000, 0);
    uint64_t after = aether_exec_account_used(a);
    aether_exec_use(NULL, NULL);
    if (r) free(r);
    aether_exec_account_free(a);
    ASSERT_TRUE(charged >= 1000);
    ASSERT_NULL(r);
    ASSERT_EQ(charged - 1000, after);
}
