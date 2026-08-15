/*
 * Experimental: State Machine Actor Benchmark
 * 
 * This file demonstrates the "State Machine Actor" model in pure C.
 * Goal: Prove that we can run 100,000+ actors on a single thread efficiently.
 * 
 * Model:
 * - Actor = C struct (state)
 * - Behavior = Function taking (state*, message)
 * - Scheduler = Loop over array of actors
 * 
 * Usage: gcc state_machine_bench.c -O2 -o bench && ./bench
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ACTOR_COUNT 100000
#define MESSAGES_PER_ACTOR 10

// --- The "Runtime" ---

typedef enum {
    MSG_INCREMENT,
    MSG_PING
} MsgType;

typedef struct {
    MsgType type;
    int payload;
} Message;

// Simple ring buffer for actor mailbox (fixed size for benchmark speed)
#define MAILBOX_SIZE 16
typedef struct {
    Message messages[MAILBOX_SIZE];
    int head;
    int tail;
    int count;
} Mailbox;

// Abstract "Actor" definition
typedef struct Actor Actor;
typedef void (*ActorStepFunc)(Actor* self);

struct Actor {
    int id;
    int active; // 1 = runnable, 0 = waiting
    Mailbox mailbox;
    ActorStepFunc step;
    // User state below (simulated "inheritance" or just flexible struct)
    int counter_value; 
};

// Mailbox ops
int send(Actor* target, Message msg) {
    if (target->mailbox.count >= MAILBOX_SIZE) return 0; // Drop if full (benchmark simplification)
    
    target->mailbox.messages[target->mailbox.tail] = msg;
    target->mailbox.tail = (target->mailbox.tail + 1) % MAILBOX_SIZE;
    target->mailbox.count++;
    target->active = 1; // Wake up actor
    return 1;
}

int receive(Actor* self, Message* out_msg) {
    if (self->mailbox.count == 0) return 0;
    
    *out_msg = self->mailbox.messages[self->mailbox.head];
    self->mailbox.head = (self->mailbox.head + 1) % MAILBOX_SIZE;
    self->mailbox.count--;
    return 1;
}

// --- The "User Code" (Compiled Aether Code) ---

// This represents the compiled code for:
// actor Counter { 
//   state int val = 0; 
//   receive(msg) { val += 1; } 
// }
void counter_actor_step(Actor* self) {
    Message msg;
    // "Blocking" receive check
    // In a real state machine, this would be a "case STATE_WAITING:"
    if (!receive(self, &msg)) {
        self->active = 0; // Yield / Sleep
        return;
    }

    // Message Handler
    if (msg.type == MSG_INCREMENT) {
        self->counter_value += 1;
        // Logic to stop?
    }
}

// --- The Scheduler ---

Actor* actors;

int main() {
    printf("Allocating %d actors...\n", ACTOR_COUNT);
    actors = malloc(sizeof(Actor) * ACTOR_COUNT);
    
    // Initialize actors
    for (int i = 0; i < ACTOR_COUNT; i++) {
        actors[i].id = i;
        actors[i].active = 1; // Start active
        actors[i].step = counter_actor_step;
        actors[i].counter_value = 0;
        actors[i].mailbox.head = 0;
        actors[i].mailbox.tail = 0;
        actors[i].mailbox.count = 0;
    }

    printf("Starting benchmark: Sending %d messages...\n", ACTOR_COUNT * MESSAGES_PER_ACTOR);
    clock_t start = clock();

    // Simulation Loop
    // In a real scenario, this is the Worker Thread loop
    int total_processed = 0;

    // Seed messages
    for (int i = 0; i < ACTOR_COUNT; i++) {
        for (int m = 0; m < MESSAGES_PER_ACTOR; m++) {
            Message msg = { .type = MSG_INCREMENT, .payload = 1 };
            send(&actors[i], msg);
        }
    }

    while (total_processed < ACTOR_COUNT * MESSAGES_PER_ACTOR) {
        int active_count = 0;
        for (int i = 0; i < ACTOR_COUNT; i++) {
            if (actors[i].active) {
                // Determine if it actually has work (optimization)
                if (actors[i].mailbox.count > 0) {
                    actors[i].step(&actors[i]);
                    total_processed++;
                    active_count++;
                } else {
                    actors[i].active = 0;
                }
            }
        }
        if (active_count == 0 && total_processed < ACTOR_COUNT * MESSAGES_PER_ACTOR) {
            // Should not happen in this dense benchmark
            break; 
        }
    }

    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Done.\n");
    printf("Processed %d messages in %.4f seconds.\n", total_processed, time_spent);
    printf("Throughput: %.0f messages/sec\n", total_processed / time_spent);
    
    // Verification
    long long total_count = 0;
    for (int i = 0; i < ACTOR_COUNT; i++) {
        total_count += actors[i].counter_value;
    }
    printf("Total State Value: %lld (Expected: %lld)\n", total_count, (long long)ACTOR_COUNT * MESSAGES_PER_ACTOR);

    free(actors);
    return 0;
}

