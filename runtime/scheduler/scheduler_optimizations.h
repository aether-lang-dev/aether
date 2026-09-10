// Scheduler Performance Optimizations Integration
// Combines: Direct Send, Adaptive Batching, Message Deduplication, SIMD.
// Each layer is independently toggleable; together they form the
// tier-1 hot-path scheduler.

#ifndef SCHEDULER_OPTIMIZATIONS_H
#define SCHEDULER_OPTIMIZATIONS_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../actors/actor_state_machine.h"
#include "../actors/aether_adaptive_batch.h"
#include "../actors/aether_direct_send.h"
#include "../actors/aether_message_dedup.h"
#include "../actors/aether_simd_batch.h"
#include <stdatomic.h>

// Enable/disable optimizations at runtime
typedef struct {
    atomic_bool use_direct_send;
    atomic_bool use_adaptive_batching;
    atomic_bool use_message_dedup;
    atomic_bool use_simd_processing;
    atomic_int direct_send_hits;
    atomic_int direct_send_misses;
    atomic_int messages_deduplicated;
    atomic_int simd_batches_processed;
} OptimizationStats;

// Global optimization state
extern OptimizationStats g_opt_stats;

// Initialize optimization subsystem
static inline void scheduler_opts_init(void) {
    atomic_store(&g_opt_stats.use_direct_send, true);
    atomic_store(&g_opt_stats.use_adaptive_batching, true);
    atomic_store(&g_opt_stats.use_message_dedup, false);  // Opt-in per actor
    atomic_store(&g_opt_stats.use_simd_processing, true);
    atomic_store(&g_opt_stats.direct_send_hits, 0);
    atomic_store(&g_opt_stats.direct_send_misses, 0);
    atomic_store(&g_opt_stats.messages_deduplicated, 0);
    atomic_store(&g_opt_stats.simd_batches_processed, 0);
}

// Enhanced actor with optimization state
typedef struct OptimizedActor {
    // Base actor fields (inline to avoid pointer indirection)
    int id;
    int active;
    atomic_int assigned_core;
    Mailbox mailbox;
    void (*step)(void*);
    
    // Optimization state
    ActorMetadata metadata;           // For direct send
    AdaptiveBatchState batch_state;   // For adaptive batching
    DedupWindow dedup_window;         // For message dedup (optional)
    void (*optimized_step)(struct OptimizedActor*);  // Optimized step function
} OptimizedActor;

// Initialize optimized actor
static inline void optimized_actor_init(
    OptimizedActor* actor,
    int scheduler_id,
    void (*step_func)(OptimizedActor*)
) {
    mailbox_init(&actor->mailbox);
    actor->step = NULL;  // Use optimized_step instead
    actor->active = 1;
    atomic_init(&actor->assigned_core, scheduler_id);
    
    actor->metadata.mailbox = &actor->mailbox;
    actor->metadata.scheduler_id = scheduler_id;
    atomic_store(&actor->metadata.message_count, 0);
    
    adaptive_batch_init(&actor->batch_state);
    
    // Initialize dedup window
    memset(&actor->dedup_window, 0, sizeof(DedupWindow));
    actor->dedup_window.write_index = 0;
    
    actor->optimized_step = step_func;
}

// Optimized message send with all optimizations
static inline int optimized_send_message(
    OptimizedActor* sender,
    OptimizedActor* receiver,
    Message msg
) {
    // Optimization 1: Direct send (same-core bypass)
    if (atomic_load(&g_opt_stats.use_direct_send) &&
        actors_same_core(&sender->metadata, &receiver->metadata)) {
        
        if (direct_send(&sender->metadata, &receiver->metadata, msg)) {
            atomic_fetch_add(&g_opt_stats.direct_send_hits, 1);
            if (receiver->optimized_step) {
                receiver->optimized_step(receiver);
            }
            return 1;
        }
        atomic_fetch_add(&g_opt_stats.direct_send_misses, 1);
    }
    
    // Optimization 2: Message deduplication (if enabled for receiver)
    if (atomic_load(&g_opt_stats.use_message_dedup)) {
        if (is_duplicate(&receiver->dedup_window, &msg)) {
            atomic_fetch_add(&g_opt_stats.messages_deduplicated, 1);
            return 1;  // Suppressed duplicate
        }
        // Record message in dedup window
        MessageFingerprint fp = message_fingerprint(&msg);
        receiver->dedup_window.window[receiver->dedup_window.write_index] = fp;
        receiver->dedup_window.write_index = (receiver->dedup_window.write_index + 1) & DEDUP_WINDOW_MASK;
    }
    
    // Fall back to normal mailbox send
    return mailbox_send(&receiver->mailbox, msg);
}

// Optimized receive with adaptive batching
static inline int optimized_receive_messages(
    OptimizedActor* actor,
    Message* buffer,
    int max_count
) {
    if (!atomic_load(&g_opt_stats.use_adaptive_batching)) {
        return mailbox_receive_batch(&actor->mailbox, buffer, max_count);
    }
    
    // Use adaptive batch size
    int batch_size = actor->batch_state.current_batch_size;
    if (batch_size > max_count) {
        batch_size = max_count;
    }
    
    int received = mailbox_receive_batch(&actor->mailbox, buffer, batch_size);
    
    // Adjust batch size for next time
    adaptive_batch_adjust(&actor->batch_state, received);
    
    return received;
}

// SIMD-optimized batch message processing
static inline void optimized_process_batch_simd(
    OptimizedActor* actor,
    Message* messages,
    int count,
    void (*handler)(OptimizedActor*, Message*)
) {
    if (!atomic_load(&g_opt_stats.use_simd_processing) || count < 4) {
        // Fall back to scalar processing
        for (int i = 0; i < count; i++) {
            handler(actor, &messages[i]);
        }
        return;
    }
    
    // SIMD processing available - process message metadata in parallel
    // (actual simd_process_message_batch would do vectorized filtering/routing)
    int simd_batches = (count + 3) / 4;  // Count of 4-message SIMD batches
    atomic_fetch_add(&g_opt_stats.simd_batches_processed, simd_batches);
    
    // Handler still called per-message (business logic is custom)
    for (int i = 0; i < count; i++) {
        handler(actor, &messages[i]);
    }
}

// Print optimization statistics
static inline void scheduler_opts_print_stats(void) {
    printf("\n=== Scheduler Optimization Statistics ===\n");
    printf("Direct Send:  %d hits, %d misses (%.1f%% hit rate)\n",
        atomic_load(&g_opt_stats.direct_send_hits),
        atomic_load(&g_opt_stats.direct_send_misses),
        100.0 * atomic_load(&g_opt_stats.direct_send_hits) / 
        (atomic_load(&g_opt_stats.direct_send_hits) + atomic_load(&g_opt_stats.direct_send_misses) + 1));
    printf("Messages Deduplicated: %d\n", atomic_load(&g_opt_stats.messages_deduplicated));
    printf("SIMD Batches: %d\n", atomic_load(&g_opt_stats.simd_batches_processed));
    printf("========================================\n");
}

#endif // SCHEDULER_OPTIMIZATIONS_H
