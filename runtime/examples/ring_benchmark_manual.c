#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "actor_state_machine.h"

#define NUM_ACTORS 1000
#define MESSAGES_PER_ACTOR 1000

typedef struct Node {
    int id;
    int active;
    Mailbox mailbox;
    int next_id;
    int count;
} Node;

void Node_step(Node* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        self->active = 0;
        return;
    }
    self->count++;
    if (self->next_id >= 0 && self->next_id < NUM_ACTORS) {
        Node** actors = (Node**)msg.payload_ptr;
        Message forward = {0};
    forward.type = 1;
    forward.sender_id = self->id;
    forward.payload_int = msg.payload_int;
    forward.payload_ptr = actors;
        mailbox_send(&actors[self->next_id]->mailbox, forward);
        actors[self->next_id]->active = 1;
    }
}

int main() {
    Node** actors = malloc(NUM_ACTORS * sizeof(Node*));
    
    for (int i = 0; i < NUM_ACTORS; i++) {
        actors[i] = malloc(sizeof(Node));
        actors[i]->id = i;
        actors[i]->active = 1;
        mailbox_init(&actors[i]->mailbox);
        actors[i]->next_id = (i + 1) % NUM_ACTORS;
        actors[i]->count = 0;
    }
    
    Message initial = {0};
    initial.type = 1;
    initial.sender_id = 0;
    initial.payload_int = 0;
    initial.payload_ptr = actors;
    mailbox_send(&actors[0]->mailbox, initial);
    actors[0]->active = 1;
    
    clock_t start = clock();
    
    int total_messages = NUM_ACTORS * MESSAGES_PER_ACTOR;
    int processed = 0;
    
    while (processed < total_messages) {
        for (int i = 0; i < NUM_ACTORS; i++) {
            if (actors[i]->active) {
                Node_step(actors[i]);
                processed++;
                if (processed >= total_messages) break;
            }
        }
    }
    
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double msg_per_sec = total_messages / elapsed;
    
    printf("Ring benchmark results:\n");
    printf("Actors: %d\n", NUM_ACTORS);
    printf("Messages: %d\n", total_messages);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f msg/sec\n", msg_per_sec);
    printf("Throughput: %.1f M msg/sec\n", msg_per_sec / 1000000.0);
    
    for (int i = 0; i < NUM_ACTORS; i++) {
        free(actors[i]);
    }
    free(actors);
    
    return 0;
}
