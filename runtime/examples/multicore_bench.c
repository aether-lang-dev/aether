#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#define usleep(us) Sleep((us)/1000)
#else
#include <unistd.h>
#endif
#include "actor_state_machine.h"
#include "multicore_scheduler.h"

#define NUM_ACTORS 2000
#define MESSAGES_PER_ACTOR 5000

typedef struct Node {
    atomic_int active;
    int id;
    Mailbox mailbox;
    void (*step)(void*);
    pthread_t thread;
    int auto_process;
    atomic_int assigned_core;
    int next_id;
    int count;
} Node;

extern __thread int current_core_id;

// Forward declaration
void send_Node(Node* actor, int type);

typedef struct {
    Node** actors;
    int actor_count;
    int messages_to_send;
    int core_id;
} WorkerArgs;

// Worker thread - sends messages only to local actors
void* worker_thread(void* arg) {
    WorkerArgs* args = (WorkerArgs*)arg;
    current_core_id = args->core_id;
    
    // Send messages only to actors on THIS core
    for (int m = 0; m < args->messages_to_send; m++) {
        int actor_idx = m % args->actor_count;
        send_Node(args->actors[actor_idx], 1);
    }
    
    return NULL;
}

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
            // Mailbox full - yield and retry
            sched_yield();
        }
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    } else {
        scheduler_send_remote((ActorBase*)actor, msg, current_core_id);
    }
}

int main() {
    int cores = 4;
    current_core_id = 0;  // Main thread acts as core 0
    
    scheduler_init(cores);
    
    Node** actors = malloc(NUM_ACTORS * sizeof(Node*));
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = spawn_Node((i + 1) % NUM_ACTORS);
    }
    
    printf("Created %d actors on %d cores\n", NUM_ACTORS, cores);
    printf("Core assignment:\n");
    for (int c = 0; c < cores; c++) {
        int count = 0;
        for (int i = 0; i < NUM_ACTORS; i++) {
            if (atomic_load_explicit(&actors[i]->assigned_core, memory_order_relaxed) == c) count++;
        }
        printf("  Core %d: %d actors\n", c, count);
    }

    // Start scheduler BEFORE sending messages
    scheduler_start();
    
    int total_messages = NUM_ACTORS * MESSAGES_PER_ACTOR;
    
    printf("Creating %d worker threads (one per core)...\n", cores);
    
    // Create worker threads - one per core to send messages locally
    pthread_t workers[4];
    WorkerArgs worker_args[4];
    
    for (int c = 0; c < cores; c++) {
        // Find actors on this core
        worker_args[c].actors = malloc(500 * sizeof(Node*));
        worker_args[c].actor_count = 0;
        for (int i = 0; i < NUM_ACTORS; i++) {
            if (atomic_load_explicit(&actors[i]->assigned_core, memory_order_relaxed) == c) {
                worker_args[c].actors[worker_args[c].actor_count++] = actors[i];
            }
        }
        worker_args[c].messages_to_send = total_messages / cores;
        worker_args[c].core_id = c;
        
        printf("Core %d: %d actors, %d messages\n", c, worker_args[c].actor_count, worker_args[c].messages_to_send);
    }
    
    printf("\nSending %d messages (truly parallel, zero cross-core traffic)...\n", total_messages);
    clock_t start = clock();
    
    // Launch worker threads - each sends only to its own core's actors
    for (int c = 0; c < cores; c++) {
        pthread_create(&workers[c], NULL, worker_thread, &worker_args[c]);
    }
    
    // Wait for all workers to finish sending
    for (int c = 0; c < cores; c++) {
        pthread_join(workers[c], NULL);
    }
    
    printf("All messages sent!\n");
    fflush(stdout);
    
    // Wait for completion
    int iterations = 0;
    while (iterations < 10000) {  // 10 second timeout
        int total_processed = 0;
        for (int i = 0; i < NUM_ACTORS; i++) {
            total_processed += actors[i]->count;
        }
        if (total_processed >= total_messages) break;
        usleep(1000);
        iterations++;
        if (iterations % 200 == 0) {
            printf("\rProcessed: %d/%d messages...", total_processed, total_messages);
            fflush(stdout);
        }
    }
    printf("\n");
    clock_t end = clock();
    
    scheduler_stop();
    scheduler_wait();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double msg_per_sec = total_messages / elapsed;
    
    int total_processed = 0;
    for (int i = 0; i < NUM_ACTORS; i++) {
        total_processed += actors[i]->count;
    }
    
    printf("\nMulti-core benchmark results:\n");
    printf("Cores: %d\n", cores);
    printf("Actors: %d\n", NUM_ACTORS);
    printf("Messages sent: %d\n", total_messages);
    // Cleanup
    for (int c = 0; c < cores; c++) {
        free(worker_args[c].actors);
    }
    
    printf("Messages processed: %d\n", total_processed);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", msg_per_sec);
    printf("Throughput: %.1f M msg/sec\n", msg_per_sec / 1000000.0);
    printf("Per-core: %.1f M msg/sec\n", (msg_per_sec / cores) / 1000000.0);
    
    for (int i = 0; i < NUM_ACTORS; i++) {
        free(actors[i]);
    }
    free(actors);
    
    return 0;
}
