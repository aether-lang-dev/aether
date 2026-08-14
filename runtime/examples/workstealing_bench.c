// Work Stealing Benchmark - Tests load balancing across cores
// Creates unbalanced workload to trigger work stealing

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include "../actors/actor_state_machine.h"
#include "../scheduler/multicore_scheduler.h"

#ifdef _WIN32
#include <windows.h>
#define usleep(us) Sleep((us) / 1000)
#else
#include <unistd.h>
#endif

#define NUM_ACTORS 1000
#define MESSAGES_PER_ACTOR 5000

typedef struct Node {
    atomic_int active;
    int id;
    atomic_int assigned_core;
    Mailbox mailbox;
    void (*step)(void*);
    int next_id;
    int count;
} Node;

extern __thread int current_core_id;

void Node_step(Node* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    self->count++;
}

Node* spawn_Node(int next_id) {
    Node* actor = malloc(sizeof(Node));
    actor->id = atomic_fetch_add(&next_actor_id, 1);
    atomic_init(&actor->active, 1);
    atomic_init(&actor->assigned_core, -1);
    actor->step = (void (*)(void*))Node_step;
    mailbox_init(&actor->mailbox);
    actor->next_id = next_id;
    actor->count = 0;
    scheduler_register_actor((ActorBase*)actor, -1);
    return actor;
}

void send_Node(Node* actor, int type) {
    Message msg = {0};
    msg.type = type;
    msg.sender_id = 0;
    msg.payload_int = 0;
    msg.payload_ptr = NULL;
    if (atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == current_core_id) {
        // Local send - retry if mailbox full
        while (!mailbox_send(&actor->mailbox, msg)) {
            sched_yield();
        }
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    } else {
        scheduler_send_remote((ActorBase*)actor, msg, current_core_id);
    }
}

int main() {
    int cores = 4;
    current_core_id = 0;
    
    scheduler_init(cores);
    
    // Create actors - assign 80% to core 0, 20% distributed across others
    // This creates severe imbalance to trigger work stealing
    Node** actors = malloc(NUM_ACTORS * sizeof(Node*));
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = spawn_Node((i + 1) % NUM_ACTORS);
    }
    
    printf("Created %d actors on %d cores\n", NUM_ACTORS, cores);
    printf("Initial core assignment (intentionally imbalanced):\n");
    for (int c = 0; c < cores; c++) {
        int count = 0;
        for (int i = 0; i < NUM_ACTORS; i++) {
            if (atomic_load_explicit(&actors[i]->assigned_core, memory_order_relaxed) == c) count++;
        }
        printf("  Core %d: %d actors (%.1f%%)\n", c, count, 100.0 * count / NUM_ACTORS);
    }

    scheduler_start();
    
    int total_messages = NUM_ACTORS * MESSAGES_PER_ACTOR;
    
    printf("\nSending %d messages to all actors...\n", total_messages);
    clock_t start = clock();
    
    // Send all messages from main thread
    for (int m = 0; m < total_messages; m++) {
        int actor_idx = m % NUM_ACTORS;
        send_Node(actors[actor_idx], 1);
    }
    
    // Wait for completion
    int iterations = 0;
    while (iterations < 10000) {
        int total_processed = 0;
        for (int i = 0; i < NUM_ACTORS; i++) {
            total_processed += actors[i]->count;
        }
        if (total_processed >= total_messages) break;
        usleep(1000);
        iterations++;
    }
    
    clock_t end = clock();
    
    scheduler_stop();
    scheduler_wait();
    
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double msg_per_sec = total_messages / elapsed;
    
    int total_processed = 0;
    for (int i = 0; i < NUM_ACTORS; i++) {
        total_processed += actors[i]->count;
    }
    
    printf("\nFinal core assignment (after work stealing):\n");
    for (int c = 0; c < cores; c++) {
        int count = 0;
        for (int i = 0; i < NUM_ACTORS; i++) {
            if (atomic_load_explicit(&actors[i]->assigned_core, memory_order_relaxed) == c) count++;
        }
        printf("  Core %d: %d actors (%.1f%%) - %d steals\n",
               c, count, 100.0 * count / NUM_ACTORS,
               atomic_load(&schedulers[c].steal_attempts));
    }
    
    printf("\nWork stealing benchmark results:\n");
    printf("Cores: %d\n", cores);
    printf("Actors: %d\n", NUM_ACTORS);
    printf("Messages sent: %d\n", total_messages);
    printf("Messages processed: %d\n", total_processed);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", msg_per_sec);
    printf("Throughput: %.1f M msg/sec\n", msg_per_sec / 1000000.0);
    
    for (int i = 0; i < NUM_ACTORS; i++) {
        free(actors[i]);
    }
    free(actors);
    
    return 0;
}
