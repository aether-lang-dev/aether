// Manual runtime test - compile and run separately
// This is NOT part of the automated test suite
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/actors/actor_state_machine.h"

typedef struct Counter {
    int id;
    atomic_int active;
    Mailbox mailbox;
    int count;
} Counter;

void Counter_step(Counter* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    (self->count = (self->count + 1));
}

Counter* spawn_Counter() {
    Counter* actor = malloc(sizeof(Counter));
    actor->id = 1;
    atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    mailbox_init(&actor->mailbox);
    actor->count = 0;
    return actor;
}

void send_message(void* actor_ptr, int type, int payload) {
    Counter* actor = (Counter*)actor_ptr;
    Message msg = {0};
    msg.type = type;
    msg.sender_id = 0;
    msg.payload_int = payload;
    msg.payload_ptr = NULL;
    mailbox_send(&actor->mailbox, msg);
    atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
}

int main() {
    Counter* c = spawn_Counter();
    
    send_message(c, 1, 0);
    Counter_step(c);
    
    printf("Counter value: %d\n", c->count);
    free(c);
    return 0;
}

