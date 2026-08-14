// Multicore Ring Benchmark with Buffered Sends
// Measures end-to-end performance with realistic message patterns

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

typedef struct RingActor {
    ActorBase base;
    struct RingActor* next;
    uint64_t count;
} RingActor;

void ring_step(void* self) {
    RingActor* actor = (RingActor*)self;
    Message msg;
    
    int processed = 0;
    while (processed < 64 && mailbox_receive(&actor->base.mailbox, &msg)) {
        actor->count++;
        
        // Forward to next actor
        if (actor->next && msg.payload_int > 0) {
            Message fwd = message_create_simple(1, actor->base.id, msg.payload_int - 1);
            send_buffered((struct ActorBase*)&actor->next->base, fwd);
        }
        processed++;
    }
    
    if (processed > 0) {
        atomic_store_explicit(&actor->base.active, 1, memory_order_relaxed);
        send_buffer_force_flush();  // Flush after processing batch
    }
}

int main() {
    printf("Multicore Ring Benchmark with Buffered Sends\n");
    printf("============================================\n\n");
    
    int num_actors = 500;
    int messages = 10000;
    int cores = 4;
    
    scheduler_init(cores);
    
    // Create ring
    RingActor* actors = malloc(sizeof(RingActor) * num_actors);
    for (int i = 0; i < num_actors; i++) {
        actors[i].count = 0;
        actors[i].base.id = i + 1;
        actors[i].base.step = ring_step;
        atomic_init(&actors[i].base.active, 1);
        mailbox_init(&actors[i].base.mailbox);
        actors[i].next = &actors[(i + 1) % num_actors];
        scheduler_register_actor(&actors[i].base, i % cores);
    }
    
    scheduler_start();
    
    // Initialize send buffer for main thread
    send_buffer_init(-1);
    
    printf("Ring: %d actors across %d cores\n", num_actors, cores);
    printf("Messages: %d\n", messages);
    printf("Total message passes: %" PRIu64 "\n\n", (uint64_t)messages * num_actors);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Inject messages
    for (int i = 0; i < messages; i++) {
        Message msg = message_create_simple(1, 0, num_actors);
        send_buffered((struct ActorBase*)&actors[0].base, msg);
        
        if (i % 1000 == 0) {
            send_buffer_force_flush();
        }
    }
    send_buffer_force_flush();
    
    // Wait for completion
    uint64_t expected = (uint64_t)messages * num_actors;
    for (int wait = 0; wait < 100; wait++) {
        uint64_t total = 0;
        for (int i = 0; i < num_actors; i++) {
            total += actors[i].count;
        }
        
        if (wait % 10 == 0) {
            printf("\rProcessed: %" PRIu64 "/%" PRIu64 " messages (%.1f%%)...", 
                   total, expected, 100.0 * total / expected);
            fflush(stdout);
        }
        
        if (total >= expected) {
            printf("\rProcessed: %" PRIu64 "/%" PRIu64 " messages (100.0%%)    \n", total, expected);
            break;
        }
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
    
    printf("\nResults:\n");
    printf("--------\n");
    printf("Time: %.3f seconds\n", elapsed);
    printf("Messages processed: %" PRIu64 "\n", total_msgs);
    printf("Throughput: %.2f M msg/sec\n", total_msgs / elapsed / 1e6);
    printf("Per-core: %.2f M msg/sec\n", total_msgs / elapsed / 1e6 / cores);
    
    free(actors);
    return 0;
}
