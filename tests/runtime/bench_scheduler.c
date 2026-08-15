/**
 * Aether Scheduler Benchmarks
 * Measures throughput, latency, and scalability
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <pthread.h>
#else
/* aether_thread.h (pulled in via the scheduler headers) provides the
 * pthread_* surface as Win32 shims; including winpthreads' <pthread.h>
 * alongside it redefines pthread_t and friends. */
#endif
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/scheduler/multicore_scheduler.h"

#ifdef _WIN32
#include <windows.h>
#define get_time_ms() GetTickCount64()
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#include <time.h>
static long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

// ============================================================================
// Benchmark Actors
// ============================================================================

typedef struct {
    int id;
    atomic_int active;
    atomic_int assigned_core;
    Mailbox mailbox;
    void (*step)(void*);
    // OPTIMIZATION: Use plain int in hot path, atomic for cross-thread reads
    int count_local;           // Fast increment in hot path (worker thread only)
    atomic_int count_visible;  // Published count (main thread reads this)
    long start_time;
    long total_latency_us;
} BenchActor;

void bench_actor_step(BenchActor* self) {
    Message msg;
    int batch_count = 0;
    
    while (mailbox_receive(&self->mailbox, &msg)) {
        self->count_local++;  // Plain increment — avoids the atomic-RMW cost on the hot path
        batch_count++;
        
        // Publish every 64 messages for cross-thread visibility
        if (batch_count >= 64) {
            atomic_store(&self->count_visible, self->count_local);
            batch_count = 0;
        }
    }
    
    // Final publish
    if (batch_count > 0) {
        atomic_store(&self->count_visible, self->count_local);
    }
    
    atomic_store_explicit(&self->active, (self->mailbox.count > 0), memory_order_relaxed);
}

// ============================================================================
// Benchmark Functions
// ============================================================================

void bench_single_core_throughput() {
    printf("\n=== Single Core Throughput ===\n");

    scheduler_init(1);

    BenchActor* actor = malloc(sizeof(BenchActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))bench_actor_step;
    actor->count_local = 0;
    atomic_store(&actor->count_visible, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    const int MSGS = 100000;
    long start = get_time_ms();
    
    for (int i = 0; i < MSGS; i++) {
        Message msg = {0};
    msg.type = 1;
    msg.sender_id = 0;
    msg.payload_int = i;
    msg.payload_ptr = NULL;
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    // Wait for processing
    while (atomic_load(&actor->count_visible) < MSGS && (get_time_ms() - start) < 5000) {
        sleep_ms(1);
    }
    
    long end = get_time_ms();
    int processed = atomic_load(&actor->count_visible);
    double elapsed = (end - start) / 1000.0;
    
    scheduler_shutdown();
    
    printf("Messages: %d / %d (%.1f%%)\n", processed, MSGS, 100.0 * processed / MSGS);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", processed / elapsed);
    
    free(actor);
    free(schedulers[0].actors);
}

void bench_multi_core_throughput(int cores) {
    printf("\n=== %d-Core Throughput ===\n", cores);
    
    scheduler_init(cores);
    
    BenchActor** actors = malloc(cores * sizeof(BenchActor*));
    for (int i = 0; i < cores; i++) {
        actors[i] = malloc(sizeof(BenchActor));
        actors[i]->id = i + 1;
        atomic_init(&actors[i]->active, 0);
        actors[i]->step = (void (*)(void*))bench_actor_step;
        actors[i]->count_local = 0;
        atomic_store(&actors[i]->count_visible, 0);
        mailbox_init(&actors[i]->mailbox);
        scheduler_register_actor((ActorBase*)actors[i], i);
    }
    
    scheduler_start();
    
    const int MSGS_PER_ACTOR = 50000;
    long start = get_time_ms();
    
    // Send messages round-robin to all actors
    for (int i = 0; i < MSGS_PER_ACTOR * cores; i++) {
        int target = i % cores;
        Message msg = {0};
    msg.type = actors[target]->id;
    msg.sender_id = 0;
    msg.payload_int = i;
    msg.payload_ptr = NULL;
        scheduler_send_remote((ActorBase*)actors[target], msg, -1);
    }
    
    // Wait for all actors to process
    int total_target = MSGS_PER_ACTOR * cores;
    while (1) {
        int total_processed = 0;
        for (int i = 0; i < cores; i++) {
            total_processed += atomic_load(&actors[i]->count_visible);
        }
        if (total_processed >= total_target || (get_time_ms() - start) > 5000) {
            break;
        }
        sleep_ms(1);
    }
    
    long end = get_time_ms();
    int total_processed = 0;
    for (int i = 0; i < cores; i++) {
        total_processed += atomic_load(&actors[i]->count_visible);
    }
    double elapsed = (end - start) / 1000.0;
    
    scheduler_shutdown();
    
    printf("Messages: %d / %d (%.1f%%)\n", total_processed, total_target, 
           100.0 * total_processed / total_target);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", total_processed / elapsed);
    printf("Per-core: %.0f msg/sec\n", total_processed / elapsed / cores);
    
    for (int i = 0; i < cores; i++) {
        free(actors[i]);
        free(schedulers[i].actors);
    }
    free(actors);
}

void bench_cross_core_overhead() {
    printf("\n=== Cross-Core Messaging Overhead ===\n");
    
    scheduler_init(4);
    
    BenchActor* actor0 = malloc(sizeof(BenchActor));
    BenchActor* actor1 = malloc(sizeof(BenchActor));
    
    actor0->id = 1;
    atomic_init(&actor0->active, 0);
    actor0->step = (void (*)(void*))bench_actor_step;
    actor0->count_local = 0;
    atomic_store(&actor0->count_visible, 0);
    mailbox_init(&actor0->mailbox);
    
    actor1->id = 2;
    atomic_init(&actor1->active, 0);
    actor1->step = (void (*)(void*))bench_actor_step;
actor1->count_local = 0;
    atomic_store(&actor1->count_visible, 0);
    mailbox_init(&actor1->mailbox);
    
    scheduler_register_actor((ActorBase*)actor0, 0);
    scheduler_register_actor((ActorBase*)actor1, 3);  // Distant core
    scheduler_start();
    
    const int MSGS = 10000;
    long start = get_time_ms();
    
    for (int i = 0; i < MSGS; i++) {
        Message msg = {0};
    msg.type = 2;
    msg.sender_id = 0;
    msg.payload_int = i;
    msg.payload_ptr = NULL;
        scheduler_send_remote((ActorBase*)actor1, msg, 0);  // From core 0 to core 3
    }
    
    while (atomic_load(&actor1->count_visible) < MSGS && (get_time_ms() - start) < 5000) {
        sleep_ms(1);
    }
    
    long end = get_time_ms();
    int processed = atomic_load(&actor1->count_visible);
    double elapsed = (end - start) / 1000.0;
    
    scheduler_shutdown();
    
    printf("Cross-core messages: %d / %d (%.1f%%)\n", processed, MSGS, 
           100.0 * processed / MSGS);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", processed / elapsed);
    
    free(actor0);
    free(actor1);
    for (int i = 0; i < 4; i++) {
        free(schedulers[i].actors);
    }
}

void bench_scalability() {
    printf("\n=== Scalability Analysis ===\n");
    printf("Cores | Throughput (msg/sec) | Efficiency\n");
    printf("------|----------------------|-----------\n");
    
    double baseline = 0;
    
    for (int cores = 1; cores <= 8; cores *= 2) {
        scheduler_init(cores);
        
        BenchActor** actors = malloc(cores * sizeof(BenchActor*));
        for (int i = 0; i < cores; i++) {
            actors[i] = malloc(sizeof(BenchActor));
            actors[i]->id = i + 1;
            atomic_init(&actors[i]->active, 0);
            actors[i]->step = (void (*)(void*))bench_actor_step;
            actors[i]->count_local = 0;
            atomic_store(&actors[i]->count_visible, 0);
            mailbox_init(&actors[i]->mailbox);
            scheduler_register_actor((ActorBase*)actors[i], i);
        }
        
        scheduler_start();
        
        const int MSGS_PER_ACTOR = 25000;
        long start = get_time_ms();
        
        for (int i = 0; i < MSGS_PER_ACTOR * cores; i++) {
            int target = i % cores;
            Message msg = {0};
    msg.type = actors[target]->id;
    msg.sender_id = 0;
    msg.payload_int = i;
    msg.payload_ptr = NULL;
            scheduler_send_remote((ActorBase*)actors[target], msg, -1);
        }
        
        int total_target = MSGS_PER_ACTOR * cores;
        while (1) {
            int total_processed = 0;
            for (int i = 0; i < cores; i++) {
                total_processed += atomic_load(&actors[i]->count_visible);
            }
            if (total_processed >= total_target || (get_time_ms() - start) > 5000) {
                break;
            }
            sleep_ms(1);
        }
        
        long end = get_time_ms();
        int total_processed = 0;
        for (int i = 0; i < cores; i++) {
            total_processed += atomic_load(&actors[i]->count_visible);
        }
        double elapsed = (end - start) / 1000.0;
        double throughput = total_processed / elapsed;
        
        if (cores == 1) {
            baseline = throughput;
        }
        
        // Efficiency: actual speedup vs ideal linear speedup
        double efficiency = baseline > 0 ? (throughput / (baseline * cores)) * 100.0 : 0.0;
        
        printf("  %d   | %15.0f        | %6.1f%%\n", cores, throughput, efficiency);
        
        scheduler_shutdown();
        
        for (int i = 0; i < cores; i++) {
            free(actors[i]);
            free(schedulers[i].actors);
        }
        free(actors);
    }
}
void bench_latency() {
    printf("\n=== Latency Test ===");
    
    scheduler_init(2);
    
    BenchActor* actor = malloc(sizeof(BenchActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))bench_actor_step;
    actor->count_local = 0;
    atomic_store(&actor->count_visible, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Send messages one at a time and measure latency
    const int SAMPLES = 1000;
    long total_latency = 0;
    int successful = 0;
    
    for (int i = 0; i < SAMPLES; i++) {
        int before = atomic_load(&actor->count_visible);
        long start = get_time_ms();
        
        Message msg = {0};
    msg.type = 1;
    msg.sender_id = 0;
    msg.payload_int = i;
    msg.payload_ptr = NULL;
        scheduler_send_remote((ActorBase*)actor, msg, -1);
        
        // Wait for this specific message to be processed
        long timeout = start + 100;
        while (atomic_load(&actor->count_visible) <= before && get_time_ms() < timeout) {
            // Tight loop for accuracy
        }
        
        long end = get_time_ms();
        if (atomic_load(&actor->count_visible) > before) {
            total_latency += (end - start);
            successful++;
        }
    }
    
    scheduler_shutdown();
    
    printf("Samples: %d / %d (%.1f%%)\n", successful, SAMPLES, 100.0 * successful / SAMPLES);
    printf("Avg latency: %.2f ms\n", successful > 0 ? (double)total_latency / successful : 0.0);
    printf("Min latency: ~%.2f ms (theoretical)\n", 0.001);
    
    free(actor);
    free(schedulers[0].actors);
    free(schedulers[1].actors);
}

void bench_contention() {
    printf("\n=== Contention Test (Many-to-One) ===");
    
    scheduler_init(4);
    
    // One target actor, multiple senders
    BenchActor* target = malloc(sizeof(BenchActor));
    target->id = 100;
    atomic_init(&target->active, 0);
    target->step = (void (*)(void*))bench_actor_step;
    target->count_local = 0;
    atomic_store(&target->count_visible, 0);
    mailbox_init(&target->mailbox);
    
    scheduler_register_actor((ActorBase*)target, 0);
    scheduler_start();
    
    const int SENDERS = 3;
    const int MSGS_PER_SENDER = 10000;
    long start = get_time_ms();
    
    // All senders bombard one actor from different cores
    for (int sender = 0; sender < SENDERS; sender++) {
        for (int i = 0; i < MSGS_PER_SENDER; i++) {
            Message msg = {0};
    msg.type = 100;
    msg.sender_id = 0;
    msg.payload_int = sender * MSGS_PER_SENDER + i;
    msg.payload_ptr = NULL;
            scheduler_send_remote((ActorBase*)target, msg, sender + 1);
        }
    }
    
    const int TOTAL = SENDERS * MSGS_PER_SENDER;
    while (atomic_load(&target->count_visible) < TOTAL && (get_time_ms() - start) < 10000) {
        sleep_ms(1);
    }
    
    long end = get_time_ms();
    int processed = atomic_load(&target->count_visible);
    double elapsed = (end - start) / 1000.0;
    
    scheduler_shutdown();
    
    printf("Senders: %d cores → 1 target\n", SENDERS);
    printf("Messages: %d / %d (%.1f%%)\n", processed, TOTAL, 100.0 * processed / TOTAL);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", processed / elapsed);
    printf("Dropped: %d (%.1f%%)\n", TOTAL - processed, 100.0 * (TOTAL - processed) / TOTAL);
    
    free(target);
    for (int i = 0; i < 4; i++) {
        free(schedulers[i].actors);
    }
}

void bench_burst_patterns() {
    printf("\n=== Burst Pattern Test ===");
    
    scheduler_init(2);
    
    BenchActor* actor = malloc(sizeof(BenchActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))bench_actor_step;
    actor->count_local = 0;
    atomic_store(&actor->count_visible, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    const int BURSTS = 10;
    const int MSGS_PER_BURST = 500;
    int total_sent = 0;
    
    long start = get_time_ms();
    
    for (int burst = 0; burst < BURSTS; burst++) {
        // Send burst
        for (int i = 0; i < MSGS_PER_BURST; i++) {
            Message msg = {0};
    msg.type = 1;
    msg.sender_id = 0;
    msg.payload_int = total_sent++;
    msg.payload_ptr = NULL;
            scheduler_send_remote((ActorBase*)actor, msg, -1);
        }
        // Let it drain
        sleep_ms(50);
    }
    
    // Wait for all to process
    while (atomic_load(&actor->count_visible) < total_sent && (get_time_ms() - start) < 10000) {
        sleep_ms(10);
    }
    
    long end = get_time_ms();
    int processed = atomic_load(&actor->count_visible);
    double elapsed = (end - start) / 1000.0;
    
    scheduler_shutdown();
    
    printf("Bursts: %d × %d messages\n", BURSTS, MSGS_PER_BURST);
    printf("Messages: %d / %d (%.1f%%)\n", processed, total_sent, 100.0 * processed / total_sent);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", processed / elapsed);
    printf("Recovery: %s\n", processed == total_sent ? "Perfect" : "Partial");
    
    free(actor);
    free(schedulers[0].actors);
    free(schedulers[1].actors);
}
void bench_mailbox_saturation() {
    printf("\n=== Mailbox Saturation Test ===\n");
    
    scheduler_init(2);
    
    BenchActor* actor = malloc(sizeof(BenchActor));
    actor->id = 1;
    atomic_init(&actor->active, 0);
    actor->step = (void (*)(void*))bench_actor_step;
    actor->count_local = 0;
    atomic_store(&actor->count_visible, 0);
    mailbox_init(&actor->mailbox);
    
    scheduler_register_actor((ActorBase*)actor, 0);
    scheduler_start();
    
    // Flood with messages to test backpressure
    const int MSGS = 5000;
    long start = get_time_ms();
    
    for (int i = 0; i < MSGS; i++) {
        Message msg = {0};
    msg.type = 1;
    msg.sender_id = 0;
    msg.payload_int = i;
    msg.payload_ptr = NULL;
        scheduler_send_remote((ActorBase*)actor, msg, -1);
    }
    
    while (atomic_load(&actor->count_visible) < MSGS && (get_time_ms() - start) < 10000) {
        sleep_ms(10);
    }
    
    long end = get_time_ms();
    int processed = atomic_load(&actor->count_visible);
    double elapsed = (end - start) / 1000.0;
    int dropped = MSGS - processed;
    
    scheduler_shutdown();
    
    printf("Messages sent: %d\n", MSGS);
    printf("Messages processed: %d (%.1f%%)\n", processed, 100.0 * processed / MSGS);
    printf("Messages dropped: %d (%.1f%%)\n", dropped, 100.0 * dropped / MSGS);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", processed / elapsed);
    
    free(actor);
    free(schedulers[0].actors);
    free(schedulers[1].actors);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("===============================================================\n");
    printf("        Aether Scheduler Performance Benchmarks               \n");
    printf("===============================================================\n");
    
    // Basic throughput
    bench_single_core_throughput();
    bench_multi_core_throughput(2);
    bench_multi_core_throughput(4);
    
    // Latency and overhead
    bench_latency();
    bench_cross_core_overhead();
    
    // Stress scenarios
    bench_contention();
    bench_burst_patterns();
    bench_mailbox_saturation();
    
    // Scalability analysis
    bench_scalability();
    
    printf("\n===============================================================\n");
    printf("                    Benchmarks Complete                        \n");
    printf("===============================================================\n");
    
    return 0;
}





