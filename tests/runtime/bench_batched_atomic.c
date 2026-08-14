/**
 * Standalone Benchmark: Batched Atomic Updates
 * Shows the performance improvement from batching atomic operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#include <time.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#include "micro_profile.h"

/* One portable counter for every bench here; see micro_profile.h. */
#define rdtsc() read_cycles()

#define MESSAGES 10000000

// Old approach: Atomic every increment (SLOW)
typedef struct {
    atomic_int count;
    int active;
} OldActor;

void* old_actor_thread(void* arg) {
    OldActor* actor = (OldActor*)arg;
    
    for (int i = 0; i < MESSAGES; i++) {
        atomic_fetch_add(&actor->count, 1);  // Atomic EVERY time
    }
    
    return NULL;
}

// New approach: Batched atomic updates (FAST)
typedef struct {
    int count_local;           // Plain int - hot path
    atomic_int count_visible;  // Atomic - cross-thread
    int active;
} NewActor;

void* new_actor_thread(void* arg) {
    NewActor* actor = (NewActor*)arg;
    
    for (int i = 0; i < MESSAGES; i++) {
        actor->count_local++;  // Plain int - FAST!
        
        // Publish every 64 messages
        if ((i & 63) == 0) {
            atomic_store(&actor->count_visible, actor->count_local);
        }
    }
    
    // Final publish
    atomic_store(&actor->count_visible, actor->count_local);
    
    return NULL;
}

int main() {
    printf("=== Batched Atomic Optimization Benchmark ===\n");
    printf("Messages: %d\n\n", MESSAGES);
    
    // Test 1: Old approach (atomic every time)
    {
        OldActor actor = {0, 1};
        pthread_t thread;
        
        uint64_t start = rdtsc();
        pthread_create(&thread, NULL, old_actor_thread, &actor);
        pthread_join(thread, NULL);
        uint64_t end = rdtsc();
        
        uint64_t cycles = end - start;
        double cycles_per_msg = (double)cycles / MESSAGES;
        
        printf("OLD: Atomic every increment\n");
        printf("  Total cycles:   %llu\n", (unsigned long long)cycles);
        printf("  Cycles/msg:     %.2f\n", cycles_per_msg);
        printf("  Messages:       %d\n", atomic_load(&actor.count));
        printf("  Throughput:     %.0f M msg/sec (@3GHz)\n\n",
               3000.0 / cycles_per_msg);
    }
    
    // Test 2: New approach (batched atomic)
    {
        NewActor actor = {0, 0, 1};
        pthread_t thread;
        
        uint64_t start = rdtsc();
        pthread_create(&thread, NULL, new_actor_thread, &actor);
        pthread_join(thread, NULL);
        uint64_t end = rdtsc();
        
        uint64_t cycles = end - start;
        double cycles_per_msg = (double)cycles / MESSAGES;
        
        printf("NEW: Batched atomic (publish every 64)\n");
        printf("  Total cycles:   %llu\n", (unsigned long long)cycles);
        printf("  Cycles/msg:     %.2f\n", cycles_per_msg);
        printf("  Messages:       %d\n", atomic_load(&actor.count_visible));
        printf("  Throughput:     %.0f M msg/sec (@3GHz)\n\n",
               3000.0 / cycles_per_msg);
    }
    
    printf("=== Results ===\n");
    printf("✓ Batched atomic approach eliminates 63/64 atomic operations per 64 messages\n");
    printf("✓ Maintains cross-thread visibility (publishes every 64 messages)\n");
    printf("✓ Hot-path ratio improvement is workload-dependent — rerun on your target to measure\n");
    printf("\nThis optimization is LIVE in actor_state_machine.h!\n");
    
    return 0;
}
