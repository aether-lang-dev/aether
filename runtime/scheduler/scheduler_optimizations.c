// Scheduler Performance Optimizations Implementation
#include "scheduler_optimizations.h"

// Global optimization statistics
OptimizationStats g_opt_stats;

void scheduler_opts_global_init(void) {
    atomic_store(&g_opt_stats.use_direct_send, true);
    atomic_store(&g_opt_stats.use_adaptive_batching, true);
    atomic_store(&g_opt_stats.use_message_dedup, false);
    atomic_store(&g_opt_stats.use_simd_processing, true);
    atomic_store(&g_opt_stats.direct_send_hits, 0);
    atomic_store(&g_opt_stats.direct_send_misses, 0);
    atomic_store(&g_opt_stats.messages_deduplicated, 0);
    atomic_store(&g_opt_stats.simd_batches_processed, 0);
}
