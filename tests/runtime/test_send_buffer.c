// Test suite for the thread-local send buffer (runtime/actors/aether_send_buffer.c).
//
// Coverage motivation: gcov on `make ci-coverage` reported
// runtime/actors/aether_send_buffer.c at 0% (0 of 34 executable lines hit)
// — the public surface (`send_buffer_flush`, `send_buffered`,
// `send_buffer_force_flush`) is called only from runtime/examples/*.c
// bench programs, which aren't part of the test suite. The codegen never
// emits these calls. So the whole batched-send path was a CI dead zone
// — any regression would have shipped silently.
//
// What this exercises:
//   1. Empty-buffer flush (early return when count == 0)
//   2. NULL-target flush (early return when target == NULL)
//   3. Single-message same-core flush (SPSC fast path)
//   4. Multi-message same-core flush (SPSC batch)
//   5. send_buffered helper accumulates without flushing
//   6. send_buffered triggers flush on target change
//   7. send_buffered triggers flush at SEND_BUFFER_SIZE
//   8. send_buffer_force_flush drains a partial buffer
//   9. send_buffer_pending reports the pending count
//
// Cross-core path is left untested here because it requires Scheduler
// state (`schedulers[]` array) — that's a separate set of tests
// already covered by test_scheduler.c. The same-core path covered
// here is the more common scenario in real Aether programs anyway.

#include "test_harness.h"
#include "../../runtime/actors/aether_send_buffer.h"
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/actors/aether_spsc_queue.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include <stdlib.h>
#include <string.h>

// Helper: build a minimal ActorBase that send_buffer_flush can target
// for the same-core fast path. Caller owns the returned pointer and
// must `free` it (plus any spsc_queue it lazily acquires).
static ActorBase* make_same_core_actor(int core_id) {
    ActorBase* actor = calloc(1, sizeof(ActorBase));
    if (!actor) return NULL;
    actor->id = 1;
    mailbox_init(&actor->mailbox);
    atomic_store(&actor->assigned_core, core_id);
    atomic_store(&actor->active, 0);
    actor->spsc_queue = NULL;  // lazy-allocated by send_buffer_flush
    return actor;
}

static void free_actor(ActorBase* actor) {
    if (!actor) return;
    if (actor->spsc_queue) free(actor->spsc_queue);
    free(actor);
}

// Helper: reset thread-local send buffer to a known state with
// core_id = 0 so the same-core path is the one we exercise.
static void reset_send_buffer(int core_id) {
    g_send_buffer.target = NULL;
    g_send_buffer.count = 0;
    g_send_buffer.core_id = core_id;
}

// Test 1: send_buffer_flush is a no-op when the buffer is empty.
TEST_CATEGORY(send_buffer_flush_empty_is_noop, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    // No actor target, no messages — must not crash, must not write
    // anything anywhere.
    send_buffer_flush();
    ASSERT_EQ(0, g_send_buffer.count);
    ASSERT_TRUE(g_send_buffer.target == NULL);
}

// Test 2: send_buffer_flush is a no-op when target is NULL even with
// count > 0 (defensive — shouldn't normally happen in practice).
TEST_CATEGORY(send_buffer_flush_null_target_is_noop, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    g_send_buffer.target = NULL;
    g_send_buffer.count = 3;
    g_send_buffer.buffer[0] = message_create_simple(1, 0, 100);
    send_buffer_flush();
    // count is left as-is — early return doesn't reset.
    ASSERT_EQ(3, g_send_buffer.count);
}

// Test 3: single-message flush via the same-core SPSC path.
TEST_CATEGORY(send_buffer_flush_single_message_spsc, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    g_send_buffer.target = actor;
    g_send_buffer.count = 1;
    g_send_buffer.buffer[0] = message_create_simple(7, 0, 42);

    send_buffer_flush();

    // Successful SPSC enqueue clears the buffer and marks actor active.
    ASSERT_EQ(0, g_send_buffer.count);
    ASSERT_EQ(1, atomic_load(&actor->active));
    ASSERT_NOT_NULL(actor->spsc_queue);  // lazy-allocated on first flush

    free_actor(actor);
}

// Test 4: multi-message batch flush.
TEST_CATEGORY(send_buffer_flush_batch_messages, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    g_send_buffer.target = actor;
    g_send_buffer.count = 5;
    for (int i = 0; i < 5; i++) {
        g_send_buffer.buffer[i] = message_create_simple(1, 0, i * 10);
    }

    send_buffer_flush();

    ASSERT_EQ(0, g_send_buffer.count);
    ASSERT_EQ(1, atomic_load(&actor->active));

    free_actor(actor);
}

// Test 5: send_buffered accumulates messages without flushing while
// the target is the same and count is below SEND_BUFFER_SIZE.
TEST_CATEGORY(send_buffered_accumulates, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    for (int i = 0; i < 10; i++) {
        send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, i));
    }

    // Should sit in buffer; no flush yet.
    ASSERT_EQ(10, g_send_buffer.count);
    ASSERT_EQ(10, send_buffer_pending());
    ASSERT_TRUE(g_send_buffer.target == actor);
    ASSERT_EQ(0, atomic_load(&actor->active));  // not flushed → not activated

    free_actor(actor);
    reset_send_buffer(0);  // don't leak the dead actor pointer to next test
}

// Test 6: send_buffered flushes when the target changes.
TEST_CATEGORY(send_buffered_flushes_on_target_change, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor_a = make_same_core_actor(0);
    ActorBase* actor_b = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor_a);
    ASSERT_NOT_NULL(actor_b);

    // 3 messages to actor_a — accumulate.
    send_buffered((struct ActorBase*)actor_a, message_create_simple(1, 0, 1));
    send_buffered((struct ActorBase*)actor_a, message_create_simple(1, 0, 2));
    send_buffered((struct ActorBase*)actor_a, message_create_simple(1, 0, 3));
    ASSERT_EQ(3, g_send_buffer.count);

    // Switching target triggers flush of pending messages, then queues
    // the new one against actor_b.
    send_buffered((struct ActorBase*)actor_b, message_create_simple(2, 0, 99));

    ASSERT_EQ(1, g_send_buffer.count);                  // only the new message
    ASSERT_TRUE(g_send_buffer.target == actor_b);
    ASSERT_EQ(1, atomic_load(&actor_a->active));        // a got activated by flush
    ASSERT_EQ(0, atomic_load(&actor_b->active));        // b not flushed yet

    free_actor(actor_a);
    free_actor(actor_b);
    reset_send_buffer(0);
}

// Test 7: send_buffered flushes when buffer hits SEND_BUFFER_SIZE.
TEST_CATEGORY(send_buffered_flushes_on_capacity, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    // Fill to capacity. Each call goes through the
    // count >= SEND_BUFFER_SIZE check (false until the 256th send,
    // since count starts at 0 and we increment after appending).
    for (int i = 0; i < SEND_BUFFER_SIZE; i++) {
        send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, i));
    }
    ASSERT_EQ(SEND_BUFFER_SIZE, g_send_buffer.count);

    // The next send_buffered triggers the count >= SEND_BUFFER_SIZE
    // branch and calls send_buffer_flush(). The flush drains as many
    // as the SPSC queue (~63 slots) + the mailbox (~31 slots) can
    // hold, then memmoves the rest to the front (partial-send path).
    // Then the new message is appended. Exact count after this is
    // implementation-defined; the invariant we care about is "the
    // flush ran" — actor is now active, and count is < the 256+1
    // we'd see if no flush had happened.
    int before = g_send_buffer.count;
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 999));
    int after = g_send_buffer.count;

    // The flush was triggered: count went down from SEND_BUFFER_SIZE
    // and is now < before+1 (which would be the no-flush case where
    // the new message just got appended to a full buffer). The exact
    // post-count is implementation-defined: depends on how many slots
    // SPSC + mailbox had free, and on which path inside flush ran.
    ASSERT_TRUE(after < before + 1);                   // flush ran
    ASSERT_TRUE(after > 0);                            // partial drain — buffer wasn't fully empty
    ASSERT_TRUE(g_send_buffer.target == actor);        // target preserved
    ASSERT_NOT_NULL(actor->spsc_queue);                // SPSC was lazily allocated

    free_actor(actor);
    reset_send_buffer(0);
}

// Test 8: send_buffer_force_flush drains a partial buffer and resets target.
TEST_CATEGORY(send_buffer_force_flush_drains_partial, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 10));
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 20));
    ASSERT_EQ(2, g_send_buffer.count);

    send_buffer_force_flush();

    ASSERT_EQ(0, g_send_buffer.count);
    ASSERT_TRUE(g_send_buffer.target == NULL);
    ASSERT_EQ(1, atomic_load(&actor->active));

    free_actor(actor);
}

// Test 9: send_buffer_force_flush is a no-op when buffer is empty.
TEST_CATEGORY(send_buffer_force_flush_empty_is_noop, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    g_send_buffer.target = NULL;
    g_send_buffer.count = 0;

    send_buffer_force_flush();

    ASSERT_EQ(0, g_send_buffer.count);
    ASSERT_TRUE(g_send_buffer.target == NULL);
}

// Test 10a: SPSC-can't-fit → mailbox fallback path. Force SPSC near
// capacity (so spsc_enqueue_batch's all-or-nothing check rejects the
// whole batch), then flush. Mailbox takes a prefix; the suffix
// remains in the buffer.
//
// Each input message carries a unique payload so we can detect any
// double-send (a regression where the same message ends up in two
// sinks). Today no double-send is possible: spsc_enqueue_batch is
// all-or-nothing — it returns either `count` (all queued) or `0`
// (none queued). The mailbox then sees a buffer that nothing has
// taken from. Locking that invariant in here so a future refactor
// that switches SPSC to partial-batch semantics doesn't silently
// regress (line 39 sends from buffer[0]; if SPSC ever returns a
// partial count, those messages would get sent twice).
//
// Acceptance: every input message appears exactly once across SPSC
// + mailbox + leftover-buffer combined. No duplicates, no missing.
TEST_CATEGORY(send_buffer_flush_no_double_send, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    // Lazy-allocate SPSC up front so we can pre-fill it.
    actor->spsc_queue = calloc(1, sizeof(SPSCQueue));
    ASSERT_NOT_NULL(actor->spsc_queue);
    spsc_queue_init(actor->spsc_queue);

    // Pre-fill SPSC to within ~5 slots of capacity. Any negative
    // payload distinguishes pre-existing messages from the test's
    // input messages (which use payloads 1..N).
    int spsc_prefill = SPSC_QUEUE_SIZE - 5;
    for (int i = 0; i < spsc_prefill; i++) {
        Message msg = message_create_simple(99, 0, -1 - i);
        ASSERT_TRUE(spsc_enqueue(actor->spsc_queue, msg) == 1);
    }

    // Set up the test's send buffer with N distinct messages.
    // N is chosen large enough that SPSC + mailbox can't hold all
    // of them: 5 SPSC slots + 32 mailbox slots = 37 max in flight,
    // so 50 input messages forces the partial-send / leftover path.
    enum { N = 50 };
    g_send_buffer.target = actor;
    g_send_buffer.count = N;
    for (int i = 0; i < N; i++) {
        // Payload 1..N — positive and distinct per message.
        g_send_buffer.buffer[i] = message_create_simple(7, 0, i + 1);
    }

    send_buffer_flush();

    // Drain SPSC + mailbox + leftover buffer, count payload occurrences.
    // Index 1..N is what the test sent; anything outside that range
    // is the pre-fill noise (negative).
    int seen[N + 1];
    memset(seen, 0, sizeof(seen));

    Message drained[SPSC_QUEUE_SIZE];
    int spsc_n = spsc_dequeue_batch(actor->spsc_queue, drained, SPSC_QUEUE_SIZE);
    for (int i = 0; i < spsc_n; i++) {
        intptr_t v = drained[i].payload_int;
        if (v >= 1 && v <= N) seen[v]++;
    }

    Message mb_drained[64];
    int mb_n = mailbox_receive_batch(&actor->mailbox, mb_drained, 64);
    for (int i = 0; i < mb_n; i++) {
        intptr_t v = mb_drained[i].payload_int;
        if (v >= 1 && v <= N) seen[v]++;
    }

    for (int i = 0; i < g_send_buffer.count; i++) {
        intptr_t v = g_send_buffer.buffer[i].payload_int;
        if (v >= 1 && v <= N) seen[v]++;
    }

    // Each input message must appear exactly once across the three
    // sinks. Duplicates indicate the double-send bug.
    int duplicates = 0;
    int missing = 0;
    for (int i = 1; i <= N; i++) {
        if (seen[i] > 1) duplicates++;
        if (seen[i] == 0) missing++;
    }
    ASSERT_EQ(0, duplicates);
    ASSERT_EQ(0, missing);

    free_actor(actor);
    reset_send_buffer(0);
}

// Test 10b: SPSC rejects + mailbox fully accepts. Forces the
// mailbox-success-after-SPSC-failure branch (lines 41-43 of
// aether_send_buffer.c). SPSC pre-filled to leave fewer free slots
// than the batch needs (so spsc_enqueue_batch's all-or-nothing
// check rejects), but the batch is small enough to fit entirely
// in the mailbox (which holds 32).
TEST_CATEGORY(send_buffer_flush_mailbox_fully_accepts, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    actor->spsc_queue = calloc(1, sizeof(SPSCQueue));
    ASSERT_NOT_NULL(actor->spsc_queue);
    spsc_queue_init(actor->spsc_queue);

    // Leave only 4 free SPSC slots — any batch > 4 will be rejected.
    for (int i = 0; i < SPSC_QUEUE_SIZE - 5; i++) {
        ASSERT_TRUE(spsc_enqueue(actor->spsc_queue,
                                 message_create_simple(99, 0, -1 - i)) == 1);
    }

    // 10 messages: rejected by SPSC (need 10 slots, only 4 free),
    // accepted entirely by mailbox (10 < 32 capacity).
    g_send_buffer.target = actor;
    g_send_buffer.count = 10;
    for (int i = 0; i < 10; i++) {
        g_send_buffer.buffer[i] = message_create_simple(7, 0, i + 1);
    }

    send_buffer_flush();

    ASSERT_EQ(0, g_send_buffer.count);                  // fully drained
    ASSERT_EQ(1, atomic_load(&actor->active));          // mailbox-success path set this
    ASSERT_EQ(10, atomic_load(&actor->mailbox.count));  // all in mailbox

    free_actor(actor);
    reset_send_buffer(0);
}

// Test 11: send_buffer_pending reports the current count.
TEST_CATEGORY(send_buffer_pending_reports_count, TEST_CATEGORY_RUNTIME) {
    reset_send_buffer(0);
    ActorBase* actor = make_same_core_actor(0);
    ASSERT_NOT_NULL(actor);

    ASSERT_EQ(0, send_buffer_pending());
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 1));
    ASSERT_EQ(1, send_buffer_pending());
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 2));
    send_buffered((struct ActorBase*)actor, message_create_simple(1, 0, 3));
    ASSERT_EQ(3, send_buffer_pending());

    send_buffer_force_flush();
    ASSERT_EQ(0, send_buffer_pending());

    free_actor(actor);
}

void register_send_buffer_tests(void) {
    // Empty - tests auto-register via TEST_CATEGORY constructor on POSIX.
}
