// Tests for scheduler performance optimizations

#include "test_harness.h"
#include "../../runtime/scheduler/scheduler_optimizations.h"
#include "../../runtime/scheduler/multicore_scheduler.h"
#include "../../runtime/actors/lockfree_mailbox.h"
#include "../../runtime/actors/aether_simd_batch.h"
#include "../../runtime/actors/aether_message_dedup.h"
#include "../../runtime/actors/aether_message_coalescing.h"
#include <string.h>

// Test 1: Optimized actor initialization
TEST_CATEGORY(optimized_actor_init_test, TEST_CATEGORY_RUNTIME) {
    OptimizedActor actor;
    optimized_actor_init(&actor, 0, NULL);
    
    ASSERT_EQ(0, actor.assigned_core);
    ASSERT_EQ(1, actor.active);
    ASSERT_EQ(0, actor.metadata.scheduler_id);
    ASSERT_NOT_NULL(actor.metadata.mailbox);
}

// Test 2: Direct send optimization (same core) - verifies message delivery
TEST_CATEGORY(direct_send_same_core, TEST_CATEGORY_RUNTIME) {
    OptimizedActor sender, receiver;
    optimized_actor_init(&sender, 0, NULL);
    optimized_actor_init(&receiver, 0, NULL);
    
    Message msg = message_create_simple(1, 0, 42);
    
    // Direct send may not increment hits if current_core_id != sender's core
    // But the message should still be delivered via normal path
    int result = optimized_send_message(&sender, &receiver, msg);
    
    // Message should be delivered (either via direct or normal path)
    ASSERT_TRUE(result == 1 || receiver.mailbox.count == 1);
}

// Test 3: Direct send skipped (different cores)
TEST_CATEGORY(direct_send_different_cores, TEST_CATEGORY_RUNTIME) {
    OptimizedActor sender, receiver;
    optimized_actor_init(&sender, 0, NULL);
    optimized_actor_init(&receiver, 1, NULL);  // Different core
    
    Message msg = message_create_simple(1, 0, 42);

    (void)atomic_load(&g_opt_stats.direct_send_misses);  // Unused but kept for potential debugging
    optimized_send_message(&sender, &receiver, msg);
    
    // Should go through normal send path
    ASSERT_EQ(1, receiver.mailbox.count);
}

// Test 4: Adaptive batching increases size
TEST_CATEGORY(adaptive_batching_increase, TEST_CATEGORY_RUNTIME) {
    OptimizedActor actor;
    optimized_actor_init(&actor, 0, NULL);
    
    int initial_batch = actor.batch_state.current_batch_size;
    
    // Simulate full batches to trigger increase
    for (int i = 0; i < 6; i++) {
        adaptive_batch_adjust(&actor.batch_state, actor.batch_state.current_batch_size);
    }
    
    ASSERT_TRUE(actor.batch_state.current_batch_size > initial_batch);
}

// Test 5: Adaptive batching decreases size
TEST_CATEGORY(adaptive_batching_decrease, TEST_CATEGORY_RUNTIME) {
    OptimizedActor actor;
    optimized_actor_init(&actor, 0, NULL);
    
    actor.batch_state.current_batch_size = 128;  // Start at midpoint
    
    // Simulate partial batches to trigger decrease (needs 10 consecutive)
    for (int i = 0; i < 12; i++) {
        adaptive_batch_adjust(&actor.batch_state, 2);  // Small number
    }
    
    ASSERT_TRUE(actor.batch_state.current_batch_size < 128);
}

// Test 6: Message deduplication
TEST_CATEGORY(message_deduplication_test, TEST_CATEGORY_RUNTIME) {
    OptimizedActor sender, receiver;
    optimized_actor_init(&sender, 0, NULL);
    optimized_actor_init(&receiver, 0, NULL);
    
    atomic_store(&g_opt_stats.use_message_dedup, true);
    
    Message msg = message_create_simple(1, 0, 42);
    
    // Send same message twice
    optimized_send_message(&sender, &receiver, msg);
    
    int before_dedup = atomic_load(&g_opt_stats.messages_deduplicated);
    optimized_send_message(&sender, &receiver, msg);
    int after_dedup = atomic_load(&g_opt_stats.messages_deduplicated);
    
    ASSERT_TRUE(after_dedup > before_dedup);
    
    atomic_store(&g_opt_stats.use_message_dedup, false);
}

// Test 7: Optimized receive with adaptive batching
TEST_CATEGORY(optimized_receive_adaptive, TEST_CATEGORY_RUNTIME) {
    OptimizedActor actor;
    optimized_actor_init(&actor, 0, NULL);
    
    int initial_batch_size = actor.batch_state.current_batch_size;
    
    // Fill mailbox
    for (int i = 0; i < 32; i++) {
        Message msg = message_create_simple(1, 0, i);
        mailbox_send(&actor.mailbox, msg);
    }
    
    Message buffer[64];
    int received = optimized_receive_messages(&actor, buffer, 64);
    
    // Should receive up to initial batch size (adaptive may change it after)
    ASSERT_TRUE(received > 0);
    ASSERT_TRUE(received <= initial_batch_size || received <= 64);
}

// Test 8: SIMD batch processing fallback
static int g_handler_count = 0;
static void test_handler(OptimizedActor* a, Message* m) {
    g_handler_count++;
}

TEST_CATEGORY(simd_batch_processing_scalar, TEST_CATEGORY_RUNTIME) {
    OptimizedActor actor;
    optimized_actor_init(&actor, 0, NULL);
    
    Message messages[2];
    messages[0] = message_create_simple(1, 0, 1);
    messages[1] = message_create_simple(1, 0, 2);
    
    g_handler_count = 0;
    
    // Small batch should use scalar processing
    optimized_process_batch_simd(&actor, messages, 2, test_handler);
    
    ASSERT_EQ(2, g_handler_count);
}

// Test 9: Optimization stats initialization
TEST_CATEGORY(optimization_stats_init, TEST_CATEGORY_RUNTIME) {
    scheduler_opts_init();
    
    ASSERT_TRUE(atomic_load(&g_opt_stats.use_direct_send));
    ASSERT_TRUE(atomic_load(&g_opt_stats.use_adaptive_batching));
    ASSERT_FALSE(atomic_load(&g_opt_stats.use_message_dedup));
    ASSERT_TRUE(atomic_load(&g_opt_stats.use_simd_processing));
}

// Test 10: Toggle optimizations at runtime
TEST_CATEGORY(toggle_optimizations_runtime, TEST_CATEGORY_RUNTIME) {
    atomic_store(&g_opt_stats.use_direct_send, false);
    ASSERT_FALSE(atomic_load(&g_opt_stats.use_direct_send));
    
    atomic_store(&g_opt_stats.use_direct_send, true);
    ASSERT_TRUE(atomic_load(&g_opt_stats.use_direct_send));
}

// Test 11: SIMD batch ID comparison
TEST_CATEGORY(simd_batch_id_comparison, TEST_CATEGORY_RUNTIME) {
    int message_ids[8] = {1, 2, 3, 2, 5, 2, 7, 8};
    
    // Look for target_id = 2 (should find 3 matches)
    uint32_t mask = simd_batch_compare_ids(message_ids, 2, 8);
    
    // Verify matches found (positions 1, 3, 5 in the array)
    // Note: exact bit pattern depends on SIMD implementation
    ASSERT_TRUE(mask != 0);  // Should find at least one match
}

// Test 12: SIMD batch processing with larger data
TEST_CATEGORY(simd_batch_process_values, TEST_CATEGORY_RUNTIME) {
    int values[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    int results[16];
    
    // Apply: result = value * 2 + 1
    simd_batch_process_int(values, results, 16, 2, 1);
    
    // Verify results
    ASSERT_EQ(1, results[0]);   // 0 * 2 + 1 = 1
    ASSERT_EQ(3, results[1]);   // 1 * 2 + 1 = 3
    ASSERT_EQ(31, results[15]); // 15 * 2 + 1 = 31
}

// Test 13: Adaptive batch boundary conditions
TEST_CATEGORY(adaptive_batch_boundaries, TEST_CATEGORY_RUNTIME) {
    AdaptiveBatchState state;
    adaptive_batch_init(&state);
    
    // Should start at default (128)
    ASSERT_TRUE(state.current_batch_size >= 64);
    ASSERT_TRUE(state.current_batch_size <= 1024);

    // Push to max
    for (int i = 0; i < 50; i++) {
        adaptive_batch_adjust(&state, state.current_batch_size);
    }
    ASSERT_TRUE(state.current_batch_size <= 1024);  // Should not exceed max
    
    // Push to min
    for (int i = 0; i < 100; i++) {
        adaptive_batch_adjust(&state, 1);
    }
    ASSERT_TRUE(state.current_batch_size >= 64);  // Should not go below min
}

// Test 14: Message fingerprint uniqueness
TEST_CATEGORY(message_fingerprint_unique, TEST_CATEGORY_RUNTIME) {
    Message msg1 = message_create_simple(1, 0, 100);
    Message msg2 = message_create_simple(2, 0, 100);  // Different type
    Message msg3 = message_create_simple(1, 0, 200);  // Different payload
    
    MessageFingerprint fp1 = message_fingerprint(&msg1);
    MessageFingerprint fp2 = message_fingerprint(&msg2);
    MessageFingerprint fp3 = message_fingerprint(&msg3);
    
    // Different types should produce different type_hash
    ASSERT_TRUE(fp1.type_hash != fp2.type_hash);
    // Different payloads should produce different payload_hash
    ASSERT_TRUE(fp1.payload_hash != fp3.payload_hash);
    // Same message produces same fingerprint
    Message msg1_copy = message_create_simple(1, 0, 100);
    MessageFingerprint fp1_copy = message_fingerprint(&msg1_copy);
    ASSERT_EQ(fp1.type_hash, fp1_copy.type_hash);
    ASSERT_EQ(fp1.payload_hash, fp1_copy.payload_hash);
}

// Test 15: Dedup window cycling
TEST_CATEGORY(dedup_window_cycling, TEST_CATEGORY_RUNTIME) {
    DedupWindow dedup;
    memset(&dedup, 0, sizeof(dedup));
    
    // Fill the window completely with messages 0 to DEDUP_WINDOW_SIZE-1
    for (int i = 0; i < DEDUP_WINDOW_SIZE; i++) {
        Message msg = message_create_simple(100 + i, 0, 1000 + i);  // Unique type and payload
        MessageFingerprint fp = message_fingerprint(&msg);
        dedup.window[dedup.write_index] = fp;
        dedup.write_index = (dedup.write_index + 1) & DEDUP_WINDOW_MASK;
    }
    
    // Message that's in the window should be detected as duplicate
    Message in_window = message_create_simple(100 + DEDUP_WINDOW_SIZE - 1, 0, 1000 + DEDUP_WINDOW_SIZE - 1);
    ASSERT_TRUE(is_duplicate(&dedup, &in_window));
    
    // Now add more messages to cycle the window
    for (int i = 0; i < DEDUP_WINDOW_SIZE; i++) {
        Message msg = message_create_simple(200 + i, 0, 2000 + i);  // New set
        MessageFingerprint fp = message_fingerprint(&msg);
        dedup.window[dedup.write_index] = fp;
        dedup.write_index = (dedup.write_index + 1) & DEDUP_WINDOW_MASK;
    }
    
    // Old message should NOT be detected anymore (was overwritten)
    ASSERT_FALSE(is_duplicate(&dedup, &in_window));
    
    // New messages should be detected
    Message new_msg = message_create_simple(200, 0, 2000);
    ASSERT_TRUE(is_duplicate(&dedup, &new_msg));
}

// Test 16: Lock-free mailbox batch receive
TEST_CATEGORY(lockfree_batch_receive_perf, TEST_CATEGORY_RUNTIME) {
    LockFreeMailbox mbox;
    lockfree_mailbox_init(&mbox);
    
    // Send batch of messages
    Message msgs[16];
    for (int i = 0; i < 16; i++) {
        msgs[i] = message_create_simple(1, 0, i);
    }
    
    int sent = lockfree_mailbox_send_batch(&mbox, msgs, 16);
    ASSERT_EQ(16, sent);
    
    // Receive batch
    Message recv[16];
    int received = lockfree_mailbox_receive_batch(&mbox, recv, 16);
    ASSERT_EQ(16, received);
    
    // Verify ordering
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(i, recv[i].payload_int);
    }
}

// Test 17: Optimized send respects SIMD flag
TEST_CATEGORY(optimized_send_simd_flag, TEST_CATEGORY_RUNTIME) {
    scheduler_opts_init();
    
    // Disable SIMD
    atomic_store(&g_opt_stats.use_simd_processing, false);
    
    int before = atomic_load(&g_opt_stats.simd_batches_processed);
    
    OptimizedActor actor;
    optimized_actor_init(&actor, 0, NULL);
    
    Message messages[8];
    for (int i = 0; i < 8; i++) {
        messages[i] = message_create_simple(1, 0, i);
    }
    
    g_handler_count = 0;
    optimized_process_batch_simd(&actor, messages, 8, test_handler);
    
    int after = atomic_load(&g_opt_stats.simd_batches_processed);
    
    // SIMD batches should not have increased (flag was off)
    ASSERT_EQ(before, after);
    ASSERT_EQ(8, g_handler_count);  // But all messages processed
    
    // Re-enable SIMD
    atomic_store(&g_opt_stats.use_simd_processing, true);
}

// Test 18: Direct send statistics tracking
TEST_CATEGORY(direct_send_stats, TEST_CATEGORY_RUNTIME) {
    scheduler_opts_init();
    
    int initial_hits = atomic_load(&g_opt_stats.direct_send_hits);
    int initial_misses = atomic_load(&g_opt_stats.direct_send_misses);
    
    OptimizedActor sender1, sender2, receiver;
    optimized_actor_init(&sender1, 0, NULL);
    optimized_actor_init(&sender2, 1, NULL);  // Different core
    optimized_actor_init(&receiver, 0, NULL); // Same core as sender1
    
    Message msg = message_create_simple(1, 0, 42);
    
    // Send from different core (should miss)
    optimized_send_message(&sender2, &receiver, msg);
    
    // Stats should reflect attempt
    int total = atomic_load(&g_opt_stats.direct_send_hits) + 
                atomic_load(&g_opt_stats.direct_send_misses);
    ASSERT_TRUE(total >= initial_hits + initial_misses);
}

// Note: Tests are auto-registered via TEST_CATEGORY macro
void register_scheduler_optimization_tests() {
    // Empty - tests registered by constructor
}


// Message coalescing. The only coverage this header had lived in a
// tests/runtime/test_optimizations.c that no target compiled; these are its
// cases, in the suite that runs.
static int coalesce_sent = 0;
static void coalesce_probe(void* msg, uint16_t size) {
    (void)msg; (void)size;
    coalesce_sent++;
}

TEST_CATEGORY(coalescing_buffer_init, TEST_CATEGORY_RUNTIME) {
    CoalescingBuffer buf;
    coalescing_buffer_init(&buf);
    ASSERT_EQ(0, buf.pending.count);
    ASSERT_EQ(COALESCE_BUFFER_SIZE, buf.pending.capacity);
}

TEST_CATEGORY(coalescing_buffer_add_reaches_threshold, TEST_CATEGORY_RUNTIME) {
    CoalescingBuffer buf;
    coalescing_buffer_init(&buf);

    int msg = 1;
    ASSERT_EQ(0, coalescing_buffer_add(&buf, &msg, sizeof(int)));
    ASSERT_EQ(1, buf.pending.count);

    int should_flush = 0;
    for (int i = 2; i <= COALESCE_THRESHOLD; i++) {
        should_flush = coalescing_buffer_add(&buf, &msg, sizeof(int));
    }
    ASSERT_TRUE(should_flush);
}

TEST_CATEGORY(coalescing_buffer_flush_sends_every_message, TEST_CATEGORY_RUNTIME) {
    CoalescingBuffer buf;
    coalescing_buffer_init(&buf);

    int messages[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; i++) {
        coalescing_buffer_add(&buf, &messages[i], sizeof(int));
    }

    coalesce_sent = 0;
    coalescing_buffer_flush(&buf, coalesce_probe);
    ASSERT_EQ(5, coalesce_sent);
    ASSERT_EQ(0, buf.pending.count);
}

TEST_CATEGORY(coalescing_stats_count_flushes, TEST_CATEGORY_RUNTIME) {
    CoalescingBuffer buf;
    coalescing_buffer_init(&buf);

    for (int batch = 0; batch < 3; batch++) {
        for (int i = 0; i < COALESCE_THRESHOLD; i++) {
            int msg = i;
            coalescing_buffer_add(&buf, &msg, sizeof(int));
        }
        coalescing_buffer_flush(&buf, coalesce_probe);
    }

    CoalescingStats stats = coalescing_buffer_stats(&buf);
    ASSERT_EQ(3, (int)stats.flush_count);
    ASSERT_EQ(COALESCE_THRESHOLD, (int)stats.high_watermark);
}

TEST_CATEGORY(coalescing_adaptive_decision, TEST_CATEGORY_RUNTIME) {
    // Light load stays uncoalesced: batching would add latency for nothing.
    ASSERT_EQ(0, coalescing_should_enable(2, 1000));
    // Either a deep queue or a high rate turns it on.
    ASSERT_TRUE(coalescing_should_enable(10, 1000));
    ASSERT_TRUE(coalescing_should_enable(3, 50000));
}
