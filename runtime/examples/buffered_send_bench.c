// Benchmark: Sender-Side Message Batching
// Tests the throughput improvement from batching messages before sending

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x)/1000)
#define sleep(x) Sleep((x)*1000)
#endif
#include "../scheduler/multicore_scheduler.h"
#include "../actors/aether_send_buffer.h"

// Test actor that counts messages
typedef struct {
    ActorBase base;
    uint64_t count;
} CounterActor;

void counter_step(void* self) {
    CounterActor* actor = (CounterActor*)self;
    Message msg;
    
    int batch_size = 128;
    int received = 0;
    while (received < batch_size && mailbox_receive(&actor->base.mailbox, &msg)) {
        actor->count++;
        received++;
    }
    
    if (received > 0) {
        atomic_store_explicit(&actor->base.active, 1, memory_order_relaxed);  // Keep processing
    }
}

// Benchmark unbuffered sends (current implementation)
double bench_unbuffered(int num_actors, int messages_per_actor) {
    printf("\n=== Unbuffered Send Benchmark ===\n");
    
    scheduler_init(4);
    
    // Create actors
    CounterActor* actors = malloc(sizeof(CounterActor) * num_actors);
    for (int i = 0; i < num_actors; i++) {
        actors[i].count = 0;
        actors[i].base.id = i + 1;
        actors[i].base.step = counter_step;
        atomic_init(&actors[i].base.active, 1);
        mailbox_init(&actors[i].base.mailbox);
        scheduler_register_actor(&actors[i].base, i % 4);
    }

    scheduler_start();

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Send messages without buffering
    for (int i = 0; i < num_actors; i++) {
        for (int j = 0; j < messages_per_actor; j++) {
            Message msg = message_create_simple(1, 0, j);
            scheduler_send_local(&actors[i].base, msg);
        }
    }
    
    // Wait for processing
    for (int wait = 0; wait < 100; wait++) {
        uint64_t total = 0;
        for (int i = 0; i < num_actors; i++) {
            total += actors[i].count;
        }
        if (total >= (uint64_t)(num_actors * messages_per_actor)) break;
        usleep(10000);  // 10ms
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    scheduler_stop();
    scheduler_wait();
    
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    uint64_t total_msgs = 0;
    for (int i = 0; i < num_actors; i++) {
        total_msgs += actors[i].count;
    }
    
    double throughput = total_msgs / elapsed / 1e6;
    printf("Actors: %d\n", num_actors);
    printf("Messages sent: %d\n", num_actors * messages_per_actor);
    printf("Messages received: %" PRIu64 "\n", total_msgs);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.2f M msg/sec\n", throughput);
    
    free(actors);
    return throughput;
}

// Benchmark buffered sends (new implementation)
double bench_buffered(int num_actors, int messages_per_actor) {
    printf("\n=== Buffered Send Benchmark ===\n");
    
    scheduler_init(4);
    
    // Create actors
    CounterActor* actors = malloc(sizeof(CounterActor) * num_actors);
    for (int i = 0; i < num_actors; i++) {
        actors[i].count = 0;
        actors[i].base.id = i + 1;
        actors[i].base.step = counter_step;
        atomic_init(&actors[i].base.active, 1);
        mailbox_init(&actors[i].base.mailbox);
        scheduler_register_actor(&actors[i].base, i % 4);
    }

    scheduler_start();

    // Initialize send buffer for main thread
    send_buffer_init(-1);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Send messages with buffering
    for (int i = 0; i < num_actors; i++) {
        for (int j = 0; j < messages_per_actor; j++) {
            Message msg = message_create_simple(1, 0, j);
            send_buffered((struct ActorBase*)&actors[i].base, msg);
        }
        // Don't flush immediately - let batching accumulate
    }
    // Final flush
    send_buffer_force_flush();
    
    // Wait for processing
    for (int wait = 0; wait < 100; wait++) {
        uint64_t total = 0;
        for (int i = 0; i < num_actors; i++) {
            total += actors[i].count;
        }
        if (total >= (uint64_t)(num_actors * messages_per_actor)) break;
        usleep(10000);  // 10ms
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    scheduler_stop();
    scheduler_wait();
    
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    uint64_t total_msgs = 0;
    for (int i = 0; i < num_actors; i++) {
        total_msgs += actors[i].count;
    }
    
    double throughput = total_msgs / elapsed / 1e6;
    printf("Actors: %d\n", num_actors);
    printf("Messages sent: %d\n", num_actors * messages_per_actor);
    printf("Messages received: %" PRIu64 "\n", total_msgs);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.2f M msg/sec\n", throughput);
    
    free(actors);
    return throughput;
}

int main() {
    printf("Sender-Side Message Batching Benchmark\n");
    printf("======================================\n");
    
    int num_actors = 2000;
    int messages_per_actor = 5000;
    
    double unbuffered_tput = bench_unbuffered(num_actors, messages_per_actor);
    
    // Small delay between benchmarks
    sleep(1);
    
    double buffered_tput = bench_buffered(num_actors, messages_per_actor);
    
    printf("\n=== Results ===\n");
    printf("Unbuffered: %.2f M msg/sec\n", unbuffered_tput);
    printf("Buffered:   %.2f M msg/sec\n", buffered_tput);
    printf("Speedup:    %.2fx\n", buffered_tput / unbuffered_tput);
    
    if (buffered_tput > unbuffered_tput * 1.5) {
        printf("\n✓ Significant improvement! Batching reduces atomic operations.\n");
        return 0;
    } else if (buffered_tput > unbuffered_tput) {
        printf("\n~ Modest improvement. May need tuning.\n");
        return 0;
    } else {
        printf("\n✗ No improvement. Check implementation.\n");
        return 1;
    }
}
