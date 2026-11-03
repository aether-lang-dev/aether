/* tests/runtime/test_resource_caps.c — issue #343 unit tests.
 *
 * Verifies the cap counter tracks current usage (not high-water-
 * mark), refuses past-cap allocations, saturates on under-account
 * free, and flips the deadline tripwire on schedule.
 */

#include "test_harness.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../runtime/utils/aether_thread.h"   /* pthread_create, for #3529 */
#include "../../std/bytes/aether_bytes.h"
#include "../../std/os/aether_os.h"
#include "../../std/fs/aether_fs.h"
#include "../../std/string/aether_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Reset to a known baseline between tests by lifting the cap and
 * draining whatever the prior test accumulated. The counter doesn't
 * expose a "set_used" API on purpose (tests manage their own
 * accounting), so we read + drain. */
static void caps_reset(void) {
    aether_caps_set_memory_cap(0);
    aether_caps_set_deadline_ms(0);
    uint64_t leftover = aether_caps_used_bytes();
    if (leftover > 0) aether_caps_account_free((size_t)leftover);
}

TEST_CATEGORY(caps_cap_disabled_by_default, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* No cap set → every check passes. */
    ASSERT_EQ(1, aether_caps_check_alloc(1));
    ASSERT_EQ(1, aether_caps_check_alloc(1024 * 1024));
    aether_caps_account_free(1);
    aether_caps_account_free(1024 * 1024);
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
}

TEST_CATEGORY(caps_refuses_oversize, TEST_CATEGORY_STDLIB) {
    caps_reset();
    aether_caps_set_memory_cap(1024 * 1024);  /* 1 MiB */
    /* 2 MiB single alloc → refused. */
    ASSERT_EQ(0, aether_caps_check_alloc(2 * 1024 * 1024));
    /* Counter unchanged on refuse. */
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
    aether_caps_set_memory_cap(0);
}

TEST_CATEGORY(caps_current_usage_not_highwater, TEST_CATEGORY_STDLIB) {
    /* The whole point of #343: long-running guests that allocate +
     * free in a loop must not eventually trip the cap on cumulative
     * churn. With current-usage tracking, alloc-then-free cancels
     * exactly. */
    caps_reset();
    aether_caps_set_memory_cap(1024 * 1024);  /* 1 MiB */
    /* 100 × 8 KiB allocs interleaved with frees — total churn ~800
     * KiB but in-flight high-water ~16 KiB. Must succeed. */
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(1, aether_caps_check_alloc(8 * 1024));
        if (i & 1) aether_caps_account_free(8 * 1024);
    }
    /* 50 still outstanding × 8 KiB = 400 KiB; within the cap. */
    ASSERT_TRUE(aether_caps_used_bytes() == 50 * 8 * 1024);
    /* Drain. */
    for (int i = 0; i < 50; i++) aether_caps_account_free(8 * 1024);
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
    aether_caps_set_memory_cap(0);
}

/* #3529: without a cap each thread counts on a slot of its own, summed on
 * demand, where every allocation of every thread used to count on one
 * atomic. Blocks made on four threads and half of them freed on another
 * still add up, and a cap set afterwards checks against all of them: it
 * moves the slots into the counter it is checked against. */
#define CAPS_SPLIT_THREADS 4
#define CAPS_SPLIT_BLOCKS  500
static void* caps_split_blocks[CAPS_SPLIT_THREADS][CAPS_SPLIT_BLOCKS];
static void* caps_split_worker(void* arg) {
    void** mine = (void**)arg;
    for (int i = 0; i < CAPS_SPLIT_BLOCKS; i++) mine[i] = aether_caps_malloc(64);
    return NULL;
}

TEST_CATEGORY(caps_split_count_across_threads, TEST_CATEGORY_STDLIB) {
    caps_reset();
    uint64_t before = aether_caps_used_bytes();
    pthread_t t[CAPS_SPLIT_THREADS];
    for (int i = 0; i < CAPS_SPLIT_THREADS; i++)
        ASSERT_EQ(0, pthread_create(&t[i], NULL, caps_split_worker, caps_split_blocks[i]));
    for (int i = 0; i < CAPS_SPLIT_THREADS; i++) pthread_join(t[i], NULL);
    for (int i = 0; i < CAPS_SPLIT_THREADS; i++)
        for (int j = 0; j < CAPS_SPLIT_BLOCKS; j++) ASSERT_NOT_NULL(caps_split_blocks[i][j]);
    uint64_t all = (uint64_t)CAPS_SPLIT_THREADS * CAPS_SPLIT_BLOCKS * 64;
    ASSERT_TRUE(aether_caps_used_bytes() == before + all);

    /* Half freed here, on a thread that made none of them. */
    for (int i = 0; i < CAPS_SPLIT_THREADS; i++)
        for (int j = 0; j < CAPS_SPLIT_BLOCKS / 2; j++) {
            aether_caps_free(caps_split_blocks[i][j], 64);
            caps_split_blocks[i][j] = NULL;
        }
    uint64_t held = before + all / 2;
    ASSERT_TRUE(aether_caps_used_bytes() == held);

    /* A cap set now sees what is held: 100 bytes of room, not more. */
    aether_caps_set_memory_cap(held + 100);
    ASSERT_TRUE(aether_caps_used_bytes() == held);
    void* refused = aether_caps_malloc(200);
    ASSERT_TRUE(refused == NULL);
    void* fits = aether_caps_malloc(64);
    ASSERT_NOT_NULL(fits);
    aether_caps_free(fits, 64);
    aether_caps_set_memory_cap(0);

    for (int i = 0; i < CAPS_SPLIT_THREADS; i++)
        for (int j = CAPS_SPLIT_BLOCKS / 2; j < CAPS_SPLIT_BLOCKS; j++)
            aether_caps_free(caps_split_blocks[i][j], 64);
    ASSERT_TRUE(aether_caps_used_bytes() == before);
}

TEST_CATEGORY(caps_saturating_decrement, TEST_CATEGORY_STDLIB) {
    /* Free more than we allocated → counter saturates at 0 in
     * release. (Debug builds would assert; this test only runs the
     * release-shape path via NDEBUG=defined builds.) */
    caps_reset();
#ifdef NDEBUG
    aether_caps_account_free(100);
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
#endif
}

TEST_CATEGORY(caps_deadline_disabled_by_default, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* No deadline → tripwire returns 0. */
    ASSERT_EQ(0, aether_caps_deadline_tripped());
}

TEST_CATEGORY(caps_deadline_trips_on_schedule, TEST_CATEGORY_STDLIB) {
    caps_reset();
    aether_caps_set_deadline_ms(20);  /* 20 ms window */
    /* Immediately after arming, tripwire should not have flipped. */
    ASSERT_EQ(0, aether_caps_deadline_tripped());
    /* Sleep 50 ms — well past the deadline. */
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    /* Now tripped. */
    ASSERT_EQ(1, aether_caps_deadline_tripped());
    /* Sticky: still tripped on a follow-up read. */
    ASSERT_EQ(1, aether_caps_deadline_tripped());
    /* Reset clears the flag. */
    aether_caps_set_deadline_ms(0);
    ASSERT_EQ(0, aether_caps_deadline_tripped());
}

/* #3528: a deadline in milliseconds became nanoseconds as `ms * 1000000LL`,
 * signed overflow past about 292 years. A host passing long.max to mean "no
 * deadline" got a time in the past, and the work stopped at once; an ACTIVE
 * budget came out negative. Each of the four conversions now saturates, so
 * a deadline that far off never trips. */
TEST_CATEGORY(caps_deadline_past_the_end_of_time, TEST_CATEGORY_STDLIB) {
    caps_reset();
    aether_caps_set_deadline_ms(INT64_MAX);
    ASSERT_EQ(0, aether_caps_deadline_tripped());
    /* The first count of milliseconds whose nanoseconds do not fit. */
    aether_caps_set_deadline_ms(INT64_MAX / 1000000LL + 1);
    ASSERT_EQ(0, aether_caps_deadline_tripped());
    aether_caps_set_deadline_ms(0);

    AetherDeadlineState s = aether_caps_deadline_for_ms(INT64_MAX);
    ASSERT_TRUE(s.at_ns > aether_caps_now_ns());

    AetherExecLimits lim = { 0, INT64_MAX, AETHER_DEADLINE_ELAPSED, NULL };
    AetherExecAccount* a = aether_exec_account_new(&lim);
    ASSERT_NOT_NULL(a);
    ASSERT_TRUE(a->deadline_at_ns > aether_caps_now_ns());
    ASSERT_EQ(0, aether_exec_account_tripped(a));
    aether_exec_account_free(a);

    lim.deadline_counts = AETHER_DEADLINE_ACTIVE;
    a = aether_exec_account_new(&lim);
    ASSERT_NOT_NULL(a);
    ASSERT_TRUE(a->active_budget_ns > 0);
    aether_exec_account_free(a);
}

TEST_CATEGORY(caps_explicit_abort_sticky, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* No deadline set, but explicit abort flips the sticky flag. */
    __aether_abort_call();
    ASSERT_EQ(1, aether_caps_deadline_tripped());
    /* Cleared by re-arming the deadline. */
    aether_caps_set_deadline_ms(0);
    ASSERT_EQ(0, aether_caps_deadline_tripped());
}

/* #462 caps-audit accounting balance: std.bytes routes its growable
 * buffer + struct through aether_caps_malloc/realloc/free. The defining
 * correctness property of a caps conversion (distinct from a leak: a
 * size-mismatched free corrupts the *accounting*, not the heap) is that
 * the counter returns to baseline after create + grow + free. Repeatedly
 * build a bytes object, force several reallocations by writing at a high
 * index, and free it; used_bytes must land back where it started — no
 * drift from an over- or under-credited free. */
TEST_CATEGORY(caps_bytes_accounting_balances, TEST_CATEGORY_STDLIB) {
    caps_reset();
    uint64_t base = aether_caps_used_bytes();
    for (int rep = 0; rep < 4; rep++) {
        AetherBytes* b = aether_bytes_new(0);
        ASSERT_TRUE(b != NULL);
        for (int i = 0; i < 4096; i++) {
            ASSERT_EQ(1, aether_bytes_set(b, i, i & 0xff));  /* forces grows */
        }
        /* Mid-life the counter must have grown above baseline. */
        ASSERT_TRUE(aether_caps_used_bytes() > base);
        aether_bytes_free(b);
        /* ...and return exactly to baseline after the matched frees. */
        ASSERT_EQ(base, aether_caps_used_bytes());
    }
}

/* #462 caps-audit, std.os: os_getenv now allocates its returned value
 * through aether_caps_malloc, so a sandbox memory cap below the value
 * length refuses the read (a plugin can't exfiltrate an arbitrarily
 * large environment value past the limit). This exercises the exact
 * POSIX path converted in this change: uncapped it returns the value;
 * capped below the value size it returns NULL with the counter
 * unchanged. (os_getenv's POSIX body is the converted site; setenv is
 * POSIX, so the test is POSIX-guarded.) */
#ifndef _WIN32
TEST_CATEGORY(caps_os_getenv_denied_past_cap, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* 64-char value, comfortably larger than the tiny cap below. */
    setenv("AETHER_CAP_TEST_VAR",
           "0123456789012345678901234567890123456789012345678901234567890123", 1);

    /* Uncapped: the value is returned as a counted string, allocated
     * through the cap, and releasing it returns the counter to baseline. */
    uint64_t before = aether_caps_used_bytes();
    char* ok = os_getenv("AETHER_CAP_TEST_VAR");
    ASSERT_TRUE(ok != NULL);
    string_release(ok);
    ASSERT_EQ(before, aether_caps_used_bytes());

    /* Cap below the value length: the os_getenv allocation is refused. */
    caps_reset();
    uint64_t base = aether_caps_used_bytes();
    aether_caps_set_memory_cap(8);
    char* denied = os_getenv("AETHER_CAP_TEST_VAR");
    ASSERT_TRUE(denied == NULL);
    /* Refusal must not perturb the counter. */
    ASSERT_EQ(base, aether_caps_used_bytes());

    aether_caps_set_memory_cap(0);
    unsetenv("AETHER_CAP_TEST_VAR");
}
#endif
/* #462 caps-audit, std.fs directory listing: dir_list_raw routes the
 * DirList struct, the doubling entries array, and every strdup'd entry
 * name through aether_caps_malloc/realloc; dir_list_free releases them
 * via the new `capacity` field (array) and strlen-at-free (names). The
 * correctness property — distinct from a leak, since a size-mismatched
 * free corrupts the accounting not the heap — is that the counter
 * returns exactly to baseline after list + free. List the current
 * directory (which always has entries), confirm the counter grew, then
 * free and confirm it lands back on baseline with no drift. */
TEST_CATEGORY(caps_fs_dir_list_accounting_balances, TEST_CATEGORY_STDLIB) {
    caps_reset();
    uint64_t base = aether_caps_used_bytes();
    for (int rep = 0; rep < 4; rep++) {
        DirList* dl = dir_list_raw(".");
        ASSERT_TRUE(dl != NULL);
        /* The source tree's CWD always has entries → counter grew. */
        ASSERT_TRUE(dir_list_count(dl) > 0);
        ASSERT_TRUE(aether_caps_used_bytes() > base);
        dir_list_free(dl);
        /* Struct + array + every entry name freed with exact sizes. */
        ASSERT_EQ(base, aether_caps_used_bytes());
    }
}

/* #462 caps-audit, std.fs file handle: file_open_raw now routes the File
 * struct AND its retained path copy (a sandboxed caller can craft an
 * enormous filename) through aether_caps_malloc; file_close_raw releases both
 * with the matching sizes. The accounting must return exactly to baseline
 * after open + close — a size-mismatched free would drift the counter. */
TEST_CATEGORY(caps_fs_file_open_close_balances, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* CWD-relative so it works on Windows too (no /tmp). The test runner
     * runs from a writable directory — the dir-list test reads "." here. */
    const char* tp = "aether_caps_fopen_test.tmp";
    FILE* w = fopen(tp, "wb");
    ASSERT_TRUE(w != NULL);
    fputs("hello", w);
    fclose(w);

    uint64_t base = aether_caps_used_bytes();
    for (int rep = 0; rep < 4; rep++) {
        File* f = file_open_raw(tp, "rb");
        ASSERT_TRUE(f != NULL);
        /* The File struct + the path copy sit above baseline. */
        ASSERT_TRUE(aether_caps_used_bytes() > base);
        file_close_raw(f);
        /* ...and both are released with their exact sizes. */
        ASSERT_EQ(base, aether_caps_used_bytes());
    }
    remove(tp);
}

/* #462 caps-audit acceptance: reading a file whose size exceeds the
 * sandbox's remaining memory headroom must be refused, not OOM the host.
 * file_read_all_raw's buffer is cap-allocated (#343); with a cap set
 * below the file size the alloc is denied (returns NULL) and the counter
 * does not drift from the refused allocation. The cap surface is what a
 * plugin host installs to bound filesystem-driven memory. */
#ifndef _WIN32
TEST_CATEGORY(caps_fs_read_denied_past_cap, TEST_CATEGORY_STDLIB) {
    caps_reset();
    const char* tp = "aether_caps_read_test.tmp";  /* CWD-relative; see above */
    FILE* w = fopen(tp, "wb");
    ASSERT_TRUE(w != NULL);
    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 32; i++) fwrite(chunk, 1, sizeof(chunk), w);  /* 128 KiB */
    fclose(w);

    File* f = file_open_raw(tp, "rb");
    ASSERT_TRUE(f != NULL);

    /* Cap at current usage + 64 KiB, below the 128 KiB read buffer. The
     * read returns a counted string (#3329: this held it in a char*, which
     * GCC 15 refuses), released here if the cap let it through, and the cap
     * and the file are put back before anything is asserted. */
    uint64_t headroom = aether_caps_used_bytes();
    aether_caps_set_memory_cap(headroom + 64 * 1024);
    struct AetherString* data = file_read_all_raw(f);
    int refused = data == NULL;
    uint64_t used_after = aether_caps_used_bytes();
    aether_caps_set_memory_cap(0);
    if (data) string_release(data);
    file_close_raw(f);
    remove(tp);

    ASSERT_TRUE(refused);                   /* read refused */
    ASSERT_EQ(headroom, used_after);        /* counter not drifted */
}
#endif

/* #2956: an allocation that fails inside a library call's execution ends
 * the call. A cap-aware allocation still returns NULL to its caller, and
 * the call ends at its next safe point or its return with the outcome;
 * outside an execution nothing changes. The test hook fails the n-th
 * cap-aware allocation from now on this thread. */
#include "../../runtime/aether_exec.h"
#include "../../runtime/actors/aether_send_message.h"
#include "../../runtime/scheduler/multicore_scheduler.h"

TEST_CATEGORY(caps_alloc_failure_outside_an_execution_changes_nothing, TEST_CATEGORY_RUNTIME) {
    caps_reset();
    aether_exec_note_outcome(AETHER_OK, "");
    aether_test_fail_alloc_after(1);
    void* p = aether_caps_malloc(32);
    void* q = aether_caps_malloc(32);
    int outcome = aether_last_outcome();
    aether_caps_free(q, 32);
    ASSERT_TRUE(p == NULL);
    ASSERT_TRUE(q != NULL);
    ASSERT_EQ(AETHER_OK, outcome);
    caps_reset();
}

/* The execution's frame, as an export stub makes it, around `body`. */
static int run_in_execution(void (*body)(void)) {
    AetherJmpFrame* x = aether_exec_enter();
    if (AETHER_SIGSETJMP(x->buf, 1)) {
        aether_exec_failed();
    } else {
        body();
        aether_exec_leave();
    }
    return aether_last_outcome();
}

static void* g_alloc_result;
static void alloc_past_cap(void) {
    aether_caps_set_memory_cap(aether_caps_used_bytes() + 16);
    g_alloc_result = aether_caps_malloc(4096);
}
static void alloc_refused(void) {
    aether_test_fail_alloc_after(1);
    g_alloc_result = aether_caps_malloc(32);
}

TEST_CATEGORY(caps_alloc_failure_ends_the_execution, TEST_CATEGORY_RUNTIME) {
    caps_reset();
    int cap = run_in_execution(alloc_past_cap);
    void* past = g_alloc_result;
    caps_reset();
    int oom = run_in_execution(alloc_refused);
    void* refused = g_alloc_result;
    char text[64];
    snprintf(text, sizeof(text), "%s", aether_last_outcome_text());
    caps_reset();
    ASSERT_TRUE(past == NULL);
    ASSERT_EQ(AETHER_MEMORY_CAP, cap);
    ASSERT_TRUE(refused == NULL);
    ASSERT_EQ(AETHER_OUT_OF_MEMORY, oom);
    ASSERT_STREQ("the call ran out of memory", text);
}

/* An allocation the code cannot go on without: inside an execution the
 * call ends with AETHER_OUT_OF_MEMORY; elsewhere it is a panic with its own
 * reason, which a try catches. */
static void oom_now(void) { aether_out_of_memory("out of memory doing this"); }

TEST_CATEGORY(out_of_memory_ends_an_execution_and_panics_elsewhere, TEST_CATEGORY_RUNTIME) {
    int outcome = run_in_execution(oom_now);
    char text[64];
    snprintf(text, sizeof(text), "%s", aether_last_outcome_text());

    char caught[64] = "";
    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        oom_now();
    }
    snprintf(caught, sizeof(caught), "%s", f->reason ? f->reason : "");
    aether_try_pop();

    ASSERT_EQ(AETHER_OUT_OF_MEMORY, outcome);
    ASSERT_STREQ("the call ran out of memory", text);
    ASSERT_STREQ("out of memory doing this", caught);
}

/* A send whose payload copy fails inside an execution: nothing is queued,
 * the message's own values are released (they had moved into it), and the
 * call ends with AETHER_OUT_OF_MEMORY. The process used to stop. */
static int g_released_type = -1;
static void note_release(int type, void* payload) { (void)payload; g_released_type = type; }
static ActorBase* g_oom_actor;
static void send_refused(void) {
    /* Larger than the bytes a message carries its payload in (#3534), so
     * the send copies it to the heap, where the copy can be refused. */
    struct { int type; int value; char pad[AETHER_MSG_INLINE_BYTES]; } m = { 42, 7, { 0 } };
    aether_test_fail_alloc_after(1);
    aether_send_message(g_oom_actor, &m, sizeof(m));
}

TEST_CATEGORY(send_payload_copy_failure_ends_the_execution, TEST_CATEGORY_RUNTIME) {
    ActorBase* a = calloc(1, sizeof(ActorBase));
    ASSERT_NOT_NULL(a);
    a->id = 31;
    mailbox_init(&a->mailbox);
    a->release_message = note_release;
    g_oom_actor = a;
    g_released_type = -1;
    int outcome = run_in_execution(send_refused);
    int queued = a->mailbox.count;
    free(a);
    ASSERT_EQ(AETHER_OUT_OF_MEMORY, outcome);
    ASSERT_EQ(42, g_released_type);
    ASSERT_EQ(0, queued);
}
