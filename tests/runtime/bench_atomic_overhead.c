/**
 * Micro-Benchmark: Atomic vs Non-Atomic Overhead
 * 
 * Measures the actual cost of atomic operations in hot paths
 * to validate optimization opportunities.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "micro_profile.h"
#include "../../runtime/actors/actor_state_machine.h"

// ============================================================================
// Test: Counter Increment Overhead
// ============================================================================

void bench_counter_overhead() {
    printf("\n=== Counter Increment Overhead ===\n");
    
    const int ITERATIONS = 10000000;  // 10M iterations
    MicroBenchResults results = {0};
    
    bench_atomic_overhead(&results, ITERATIONS);
    
    double atomic_cycles = micro_profile_avg_cycles(&results.with_atomic);
    double plain_cycles = micro_profile_avg_cycles(&results.without_atomic);
    double overhead_cycles = atomic_cycles - plain_cycles;
    
    printf("Iterations: %d\n", ITERATIONS);
    printf("Plain int:  %.2f cycles/op\n", plain_cycles);
    printf("Atomic int: %.2f cycles/op\n", atomic_cycles);
    printf("Overhead:   %.2f cycles/op (%.1fx slower)\n\n", 
           overhead_cycles, atomic_cycles / plain_cycles);
    
    // Calculate impact on message throughput
    printf("Impact on message processing:\n");
    printf("  At 3 GHz CPU: %.2f ns/op overhead\n", overhead_cycles / 3.0);
    printf("  Max throughput (plain):  %.2f M ops/sec\n", 3000.0 / plain_cycles);
    printf("  Max throughput (atomic): %.2f M ops/sec\n", 3000.0 / atomic_cycles);
    printf("  Lost throughput:         %.2f M ops/sec\n", 
           3000.0 / plain_cycles - 3000.0 / atomic_cycles);
}

// ============================================================================
// Test: Mailbox Operation Overhead
// ============================================================================

void bench_mailbox_operations() {
    printf("\n=== Mailbox Operation Overhead ===\n");
    
    const int ITERATIONS = 1000000;  // 1M iterations
    Mailbox mbox;
    mailbox_init(&mbox);
    
    Message msg = {0};
    msg.type = 1;
    msg.sender_id = 0;
    msg.payload_int = 42;
    msg.payload_ptr = NULL;
    CycleTimer send_timer = {0};
    CycleTimer receive_timer = {0};
    
    // Measure send + receive round-trip
    uint64_t start = read_cycles();
    for (int i = 0; i < ITERATIONS; i++) {
        micro_profile_start(&send_timer);
        mailbox_send(&mbox, msg);
        micro_profile_end(&send_timer);
        
        Message out;
        micro_profile_start(&receive_timer);
        mailbox_receive(&mbox, &out);
        micro_profile_end(&receive_timer);
    }
    uint64_t end = read_cycles();
    uint64_t total = end - start;
    
    printf("Iterations: %d\n", ITERATIONS);
    printf("Send:       %.2f cycles/op\n", micro_profile_avg_cycles(&send_timer));
    printf("Receive:    %.2f cycles/op\n", micro_profile_avg_cycles(&receive_timer));
    printf("Round-trip: %.2f cycles/op\n", (double)total / ITERATIONS);
    printf("Throughput: %.2f M msg/sec (at 3 GHz)\n\n", 
           3000.0 * ITERATIONS / total);
}

// ============================================================================
// Test: Message Copy Overhead
// ============================================================================

void bench_message_copy() {
    printf("\n=== Message Copy Overhead ===\n");
    
    const int ITERATIONS = 10000000;
    Message src = {1, 2, 42, NULL, {NULL, 0, 0}, NULL};
    Message dst = {0};
    
    CycleTimer timer = {0};
    micro_profile_start(&timer);
    for (int i = 0; i < ITERATIONS; i++) {
        dst = src;  // Struct copy
    }
    micro_profile_end(&timer);
    
    printf("Iterations: %d\n", ITERATIONS);
    printf("Copy:       %.2f cycles/op\n", micro_profile_avg_cycles(&timer));
    printf("Message size: %zu bytes\n", sizeof(Message));
    /* Read the destination back: an unread copy is dead code the optimiser is
     * free to delete, which would leave the loop above timing nothing. */
    printf("Last payload: %lld\n", (long long)dst.payload_int);
}

// ============================================================================
// Test: Realistic Actor Processing Loop
// ============================================================================

typedef struct {
    int id;
    int active;
    Mailbox mailbox;
    int message_count;  // Plain int
} PlainActor;

typedef struct {
    int id;
    int active;
    Mailbox mailbox;
    _Atomic int message_count;  // Atomic int
} AtomicActor;

void bench_actor_loop() {
    printf("\n=== Actor Message Processing Loop ===\n");
    
    const int MESSAGES = 1000000;
    Message msg = {0};
    msg.type = 1;
    msg.sender_id = 0;
    msg.payload_int = 42;
    msg.payload_ptr = NULL;
    
    // Test with plain int
    PlainActor plain_actor = {.id = 1, .active = 1};
    mailbox_init(&plain_actor.mailbox);
    
    uint64_t plain_start = read_cycles();
    for (int i = 0; i < MESSAGES; i++) {
        mailbox_send(&plain_actor.mailbox, msg);
        Message out;
        if (mailbox_receive(&plain_actor.mailbox, &out)) {
            plain_actor.message_count++;  // Plain increment
        }
    }
    uint64_t plain_end = read_cycles();
    uint64_t plain_cycles = plain_end - plain_start;
    
    // Test with atomic int
    AtomicActor atomic_actor = {.id = 1, .active = 1};
    mailbox_init(&atomic_actor.mailbox);
    
    uint64_t atomic_start = read_cycles();
    for (int i = 0; i < MESSAGES; i++) {
        mailbox_send(&atomic_actor.mailbox, msg);
        Message out;
        if (mailbox_receive(&atomic_actor.mailbox, &out)) {
            atomic_fetch_add(&atomic_actor.message_count, 1);  // Atomic increment
        }
    }
    uint64_t atomic_end = read_cycles();
    uint64_t atomic_cycles = atomic_end - atomic_start;
    
    printf("Messages: %d\n", MESSAGES);
    printf("\nWith Plain int:\n");
    printf("  Total:      %llu cycles\n", (unsigned long long)plain_cycles);
    printf("  Per msg:    %.2f cycles\n", (double)plain_cycles / MESSAGES);
    printf("  Throughput: %.2f M msg/sec (at 3 GHz)\n", 3000.0 * MESSAGES / plain_cycles);
    
    printf("\nWith Atomic int:\n");
    printf("  Total:      %llu cycles\n", (unsigned long long)atomic_cycles);
    printf("  Per msg:    %.2f cycles\n", (double)atomic_cycles / MESSAGES);
    printf("  Throughput: %.2f M msg/sec (at 3 GHz)\n", 3000.0 * MESSAGES / atomic_cycles);
    
    double overhead = (double)atomic_cycles / plain_cycles;
    printf("\nAtomic Overhead: %.2fx slower\n", overhead);
    printf("Lost Throughput: %.2f M msg/sec\n", 
           3000.0 * MESSAGES / plain_cycles - 3000.0 * MESSAGES / atomic_cycles);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("===============================================================\n");
    printf("     Aether Micro-Benchmarks: Atomic Operation Overhead\n");
    printf("===============================================================\n");
    
#ifdef _WIN32
    print_cache_info();
#endif
    
    bench_counter_overhead();
    bench_mailbox_operations();
    bench_message_copy();
    bench_actor_loop();
    
    printf("\n===============================================================\n");
    printf("Conclusion:\n");
    printf("  - Measure actual overhead of atomics in YOUR hot paths\n");
    printf("  - Use plain int for single-threaded counters\n");
    printf("  - Use atomics only when cross-thread visibility needed\n");
    printf("===============================================================\n");
    
    return 0;
}
