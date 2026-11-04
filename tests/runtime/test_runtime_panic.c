#include "test_harness.h"
#include "../../runtime/actors/aether_panic.h"
#include "../../std/string/aether_string.h"
#include "../../runtime/aether_process_mem.h"
#include <string.h>
#include <stdint.h>
#include <pthread.h>

/* #2340: a heap-built panic message reaches the runtime as the AetherString*
 * codegen passes (so a catch can adopt it), typed const char*. Consumers that
 * want a C string -- the death hook here, the uncaught stderr print covered
 * by tests/integration/panic_uncaught_message -- must unwrap it; reading it as a
 * char* yields the header's magic bytes. */

static char g_hook_reason[128];
static int  g_hook_actor;

static void record_death(int actor_id, const char* reason) {
    g_hook_actor = actor_id;
    strncpy(g_hook_reason, reason, sizeof(g_hook_reason) - 1);
    g_hook_reason[sizeof(g_hook_reason) - 1] = '\0';
}

static void reset_hook(void) {
    g_hook_reason[0] = '\0';
    g_hook_actor = -1;
    aether_set_on_actor_death(record_death);
}

TEST_CATEGORY(death_hook_unwraps_heap_reason, TEST_CATEGORY_RUNTIME) {
    AetherString* msg = string_from_cstr("idx 5 is outside 4");
    ASSERT_NOT_NULL(msg);
    reset_hook();

    aether_fire_death_hook(7, (const char*)msg);
    aether_set_on_actor_death(NULL);
    string_release(msg);

    ASSERT_EQ(7, g_hook_actor);
    ASSERT_STREQ("idx 5 is outside 4", g_hook_reason);
}

TEST_CATEGORY(death_hook_passes_plain_reason_through, TEST_CATEGORY_RUNTIME) {
    reset_hook();

    aether_fire_death_hook(3, "literal reason");
    aether_set_on_actor_death(NULL);

    ASSERT_EQ(3, g_hook_actor);
    ASSERT_STREQ("literal reason", g_hook_reason);
}

TEST_CATEGORY(death_hook_null_reason_is_unknown, TEST_CATEGORY_RUNTIME) {
    reset_hook();

    aether_fire_death_hook(1, NULL);
    aether_set_on_actor_death(NULL);

    ASSERT_EQ(1, g_hook_actor);
    ASSERT_STREQ("unknown", g_hook_reason);
}

/* The caught path must keep handing the catcher the raw pointer it was
 * given: the catch lowering adopts it as the binding's heap string, which is
 * why only the no-frame fallback needed the unwrap. */
TEST_CATEGORY(caught_panic_keeps_raw_heap_reason, TEST_CATEGORY_RUNTIME) {
    AetherString* msg = string_from_cstr("caught heap message");
    ASSERT_NOT_NULL(msg);
    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        aether_panic_owned((const char*)msg, string_release);
    }
    bool same_reason = f->reason == (const char*)msg;
    bool same_release = f->reason_release == string_release;
    char text[128];
    snprintf(text, sizeof(text), "%s", f->reason ? aether_string_data(f->reason) : "");
    aether_try_pop();
    // Clean up before assertions: the harness longjmps on a failure.
    string_release(msg);

    ASSERT_TRUE(same_reason);
    ASSERT_TRUE(same_release);
    ASSERT_STREQ("caught heap message", text);
}

/* The unwind drain (#2956): it allocates nothing, since a panic may be the
 * interruption of a call whose allocation just failed; it releases the
 * frame's entries newest first, each taken out of the journal before its
 * release runs. */
static int g_rel_order[8];
static int g_rel_n;
static const void* g_forget_on_release;
static const void* g_track_on_release;
static void rel_note(int id) { if (g_rel_n < 8) g_rel_order[g_rel_n++] = id; }
static void rel_one(const void* p) { (void)p; rel_note(1); }
static void rel_two(const void* p) { (void)p; rel_note(2); }
static void rel_three(const void* p) { (void)p; rel_note(3); }
static void rel_forgets(const void* p) {
    (void)p;
    rel_note(9);
    aether_unwind_forget(g_forget_on_release);
}
static void rel_tracks(const void* p) {
    (void)p;
    rel_note(8);
    aether_unwind_track(g_track_on_release, rel_three);
}

TEST_CATEGORY(unwind_drain_releases_newest_first, TEST_CATEGORY_RUNTIME) {
    static int a, b, c;
    g_rel_n = 0;
    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        aether_unwind_track(&a, rel_one);
        aether_unwind_track(&b, rel_two);
        aether_unwind_track(&c, rel_three);
        aether_panic("drain");
    }
    int left = aether_unwind_journal_size();
    aether_try_pop();
    ASSERT_EQ(3, g_rel_n);
    ASSERT_EQ(3, g_rel_order[0]);
    ASSERT_EQ(2, g_rel_order[1]);
    ASSERT_EQ(1, g_rel_order[2]);
    ASSERT_EQ(0, left);
}

/* A release that forgets another entry of the frame (its free has run, or
 * its value moved): that entry is not released too. The drain used to
 * release every entry from a copy it took before the first release, so the
 * forgotten one was released a second time. */
TEST_CATEGORY(unwind_drain_skips_entry_a_release_forgets, TEST_CATEGORY_RUNTIME) {
    static int older, newer;
    g_rel_n = 0;
    g_forget_on_release = &older;
    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        aether_unwind_track(&older, rel_one);
        aether_unwind_track(&newer, rel_forgets);
        aether_panic("drain");
    }
    int left = aether_unwind_journal_size();
    aether_try_pop();
    ASSERT_EQ(1, g_rel_n);
    ASSERT_EQ(9, g_rel_order[0]);
    ASSERT_EQ(0, left);
}

/* An entry a release tracks is not the drain's: it stays in the journal
 * for the frame that receives the panic (here the outermost, which empties
 * the journal when it is popped). */
TEST_CATEGORY(unwind_drain_keeps_entry_a_release_tracks, TEST_CATEGORY_RUNTIME) {
    static int first, made;
    g_rel_n = 0;
    g_track_on_release = &made;
    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        aether_unwind_track(&first, rel_tracks);
        aether_panic("drain");
    }
    int left = aether_unwind_journal_size();
    aether_try_pop();
    ASSERT_EQ(1, g_rel_n);
    ASSERT_EQ(8, g_rel_order[0]);
    ASSERT_EQ(1, left);
    ASSERT_EQ(0, aether_unwind_journal_size());
}

/* A thread that journaled gives its journal back when it ends (#3539). Only
 * the scheduler's threads released theirs; any other thread that ran code
 * inside a frame (a host's thread calling an --emit=lib library, a worker)
 * left its journal behind, which LeakSanitizer found in
 * tests/integration/emit_lib_interrupt. Each thread here journals more than
 * the first allocation holds, so the journal is grown once too. Where the
 * heap count is exact, some round of threads leaves it as it found it. */
static int g_thread_cells[48];

static void cell_kept(const void* p) {
    (void)p;
}

static void* journal_then_end(void* arg) {
    (void)arg;
    (void)aether_try_push();
    for (int i = 0; i < 48; i++) aether_unwind_track(&g_thread_cells[i], cell_kept);
    for (int i = 0; i < 48; i++) aether_unwind_forget(&g_thread_cells[i]);
    aether_try_pop();
    return NULL;
}

static int threads_journal(int n) {
    int ran = 0;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, journal_then_end, NULL) != 0) continue;
        pthread_join(t, NULL);
        ran++;
    }
    return ran;
}

TEST_CATEGORY(unwind_journal_goes_back_with_its_thread, TEST_CATEGORY_RUNTIME) {
    ASSERT_EQ(4, threads_journal(4));
    if (!aether_heap_in_use_exact()) return;
    int64_t grown = -1;
    for (int round = 0; round < 5 && grown != 0; round++) {
        int64_t before = aether_heap_in_use();
        threads_journal(8);
        grown = aether_heap_in_use() - before;
    }
    ASSERT_EQ(0, grown);
}
