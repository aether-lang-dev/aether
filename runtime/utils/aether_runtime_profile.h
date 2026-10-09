/**
 * Aether Runtime Performance Profiling
 * 
 * Integrated profiling for continuous performance monitoring.
 * Compile with -DAETHER_PROFILE to enable.
 */

#ifndef AETHER_RUNTIME_PROFILE_H
#define AETHER_RUNTIME_PROFILE_H

#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>

/* Only profile_rdtsc below reads the time-stamp counter, and only under
 * AETHER_PROFILE. GCC and Clang have it as a builtin; MSVC declares it in
 * <intrin.h>. The header used to include <x86intrin.h> for every includer,
 * and every actor program includes this one: some 35,000 lines of AVX-512
 * and AVX10 declarations parsed per program build on x86 for a function
 * none of them compiled (#2673). */
#if defined(AETHER_PROFILE) && defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

// ============================================================================
// Profiling Statistics (Per-Core)
// ============================================================================

typedef struct {
    // Message operations
    atomic_ullong mailbox_send_cycles;
    atomic_ullong mailbox_receive_cycles;
    atomic_ullong mailbox_send_count;
    atomic_ullong mailbox_receive_count;
    
    // Batching operations
    atomic_ullong batch_send_cycles;
    atomic_ullong batch_receive_cycles;
    atomic_ullong batch_send_count;
    atomic_ullong batch_receive_count;
    
    // Cross-core messaging
    atomic_ullong queue_enqueue_cycles;
    atomic_ullong queue_dequeue_cycles;
    atomic_ullong queue_enqueue_count;
    atomic_ullong queue_dequeue_count;
    
    // Scheduler operations
    atomic_ullong actor_step_cycles;
    atomic_ullong actor_step_count;
    atomic_ullong idle_cycles_total;
    
    // SPSC queue operations
    atomic_ullong spsc_enqueue_cycles;
    atomic_ullong spsc_dequeue_cycles;
    atomic_ullong spsc_enqueue_count;
    atomic_ullong spsc_dequeue_count;
    
    // Atomic operation overhead tracking
    atomic_ullong atomic_op_count;
    atomic_ullong lock_contention_count;
} ProfileStats;

// Global per-core profiling stats
extern ProfileStats g_profile_stats[16];  // MAX_CORES

// ============================================================================
// Inline Profiling Macros
// ============================================================================

#ifdef AETHER_PROFILE

static inline uint64_t profile_rdtsc(void) {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_ia32_rdtsc();
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif defined(__aarch64__) || defined(__arm64__)
    // ARM64: Use system timer counter
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
#else
    // Fallback: use a simple counter (not cycle-accurate)
    return 0;
#endif
}

#define PROFILE_START() uint64_t _prof_start = profile_rdtsc()

#define PROFILE_END_MAILBOX_SEND(core_id) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].mailbox_send_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].mailbox_send_count, 1); \
} while(0)

#define PROFILE_END_MAILBOX_RECEIVE(core_id) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].mailbox_receive_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].mailbox_receive_count, 1); \
} while(0)

#define PROFILE_END_BATCH_SEND(core_id, count) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].batch_send_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].batch_send_count, count); \
} while(0)

#define PROFILE_END_BATCH_RECEIVE(core_id, count) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].batch_receive_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].batch_receive_count, count); \
} while(0)

#define PROFILE_END_QUEUE_ENQUEUE(core_id) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].queue_enqueue_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].queue_enqueue_count, 1); \
} while(0)

#define PROFILE_END_QUEUE_DEQUEUE(core_id) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].queue_dequeue_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].queue_dequeue_count, 1); \
} while(0)

#define PROFILE_END_ACTOR_STEP(core_id) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].actor_step_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].actor_step_count, 1); \
} while(0)

#define PROFILE_END_SPSC_ENQUEUE(core_id) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].spsc_enqueue_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].spsc_enqueue_count, 1); \
} while(0)

#define PROFILE_END_SPSC_DEQUEUE(core_id) do { \
    uint64_t _cycles = profile_rdtsc() - _prof_start; \
    atomic_fetch_add(&g_profile_stats[core_id].spsc_dequeue_cycles, _cycles); \
    atomic_fetch_add(&g_profile_stats[core_id].spsc_dequeue_count, 1); \
} while(0)

#define PROFILE_ATOMIC_OP(core_id) \
    atomic_fetch_add(&g_profile_stats[core_id].atomic_op_count, 1)

#define PROFILE_LOCK_CONTENTION(core_id) \
    atomic_fetch_add(&g_profile_stats[core_id].lock_contention_count, 1)

#define PROFILE_IDLE_CYCLES(core_id, cycles) \
    atomic_fetch_add(&g_profile_stats[core_id].idle_cycles_total, cycles)

#else
// Disabled - zero overhead in production
#define PROFILE_START() ((void)0)
#define PROFILE_END_MAILBOX_SEND(core_id) ((void)0)
#define PROFILE_END_MAILBOX_RECEIVE(core_id) ((void)0)
#define PROFILE_END_BATCH_SEND(core_id, count) ((void)0)
#define PROFILE_END_BATCH_RECEIVE(core_id, count) ((void)0)
#define PROFILE_END_QUEUE_ENQUEUE(core_id) ((void)0)
#define PROFILE_END_QUEUE_DEQUEUE(core_id) ((void)0)
#define PROFILE_END_ACTOR_STEP(core_id) ((void)0)
#define PROFILE_END_SPSC_ENQUEUE(core_id) ((void)0)
#define PROFILE_END_SPSC_DEQUEUE(core_id) ((void)0)
#define PROFILE_ATOMIC_OP(core_id) ((void)0)
#define PROFILE_LOCK_CONTENTION(core_id) ((void)0)
#define PROFILE_IDLE_CYCLES(core_id, cycles) ((void)0)
#endif

// ============================================================================
// Reporting Functions (always available)
// ============================================================================

void profile_init(void);
void profile_reset(void);
void profile_print_report(int num_cores);
void profile_print_summary(int num_cores);
void profile_dump_csv(const char* filename, int num_cores);

#endif // AETHER_RUNTIME_PROFILE_H
