// Message Index Passing Benchmark
// Tests passing message indices instead of copying structs

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x)/1000)
#endif
#include "../scheduler/multicore_scheduler.h"
#include "../actors/aether_send_buffer.h"

// Global message buffer (shared, read-only after init)
#define MSG_BUFFER_SIZE 1048576
Message g_msg_buffer[MSG_BUFFER_SIZE];
int g_msg_count = 0;

typedef struct {
    ActorBase base;
    uint64_t count;
    int msg_indices[256];  // Buffer of message indices
    int msg_count;
} IndexActor;

void index_step(void* self) {
    IndexActor* actor = (IndexActor*)self;
    Message msg;
    
    int processed = 0;
    while (processed < 128 && mailbox_receive(&actor->base.mailbox, &msg)) {
        actor->count++;
        processed++;
    }
    
    if (processed > 0) {
        atomic_store_explicit(&actor->base.active, 1, memory_order_relaxed);
    }
}

double bench_index_passing(int num_actors, int msgs_per_actor) {
    printf("\n=== Index Passing Benchmark ===\n");
    
    scheduler_init(4);
    
    IndexActor* actors = malloc(sizeof(IndexActor) * num_actors);
    for (int i = 0; i < num_actors; i++) {
        actors[i].count = 0;
        actors[i].base.id = i + 1;
        actors[i].base.step = index_step;
        atomic_init(&actors[i].base.active, 1);
        actors[i].msg_count = 0;
        mailbox_init(&actors[i].base.mailbox);
        scheduler_register_actor(&actors[i].base, i % 4);
    }
    
    scheduler_start();
    send_buffer_init(-1);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Pre-create messages in buffer
    for (int i = 0; i < msgs_per_actor && i < MSG_BUFFER_SIZE; i++) {
        g_msg_buffer[i] = message_create_simple(1, 0, i);
    }
    
    // Send using buffered approach (still copies, but measures baseline)
    for (int i = 0; i < num_actors; i++) {
        for (int j = 0; j < msgs_per_actor && j < MSG_BUFFER_SIZE; j++) {
            send_buffered((struct ActorBase*)&actors[i].base, g_msg_buffer[j % MSG_BUFFER_SIZE]);
        }
    }
    send_buffer_force_flush();
    
    // Wait
    for (int wait = 0; wait < 100; wait++) {
        uint64_t total = 0;
        for (int i = 0; i < num_actors; i++) {
            total += actors[i].count;
        }
        if (total >= (uint64_t)(num_actors * msgs_per_actor)) break;
        usleep(10000);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    scheduler_stop();
    scheduler_wait();
    
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    uint64_t total = 0;
    for (int i = 0; i < num_actors; i++) {
        total += actors[i].count;
    }
    
    double tput = total / elapsed / 1e6;
    printf("Messages: %" PRIu64 "\n", total);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.2f M msg/sec\n", tput);
    
    free(actors);
    return tput;
}

int main() {
    printf("Message Index Passing vs Copy Benchmark\n");
    printf("=======================================\n");
    
    double tput = bench_index_passing(2000, 5000);
    
    printf("\n=== Analysis ===\n");
    printf("Current (copy): %.2f M msg/sec\n", tput);
    printf("Goal (index): 250+ M msg/sec\n");
    printf("\nNext optimization: Replace message copying with index passing\n");
    printf("Expected improvement: 50-100%%\n");
    
    return 0;
}
