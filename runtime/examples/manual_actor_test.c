#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "actor_state_machine.h"

typedef struct Counter {
    int id;
    int active;
    Mailbox mailbox;
    int count;
} Counter;

void Counter_step(Counter* self) {
    Message msg;
    if (!mailbox_receive(&self->mailbox, &msg)) {
        self->active = 0;
        return;
    }
    (self->count = (self->count + 1));
}

Counter* spawn_Counter() {
    Counter* actor = malloc(sizeof(Counter));
    actor->id = 0;
    actor->active = 1;
    mailbox_init(&actor->mailbox);
    actor->count = 0;
    return actor;
}

void send_Counter(Counter* actor, int type, int payload) {
    Message msg = {0};
    msg.type = type;
    msg.sender_id = 0;
    msg.payload_int = payload;
    msg.payload_ptr = NULL;
    mailbox_send(&actor->mailbox, msg);
    actor->active = 1;
}

int main() {
    Counter* c1 = spawn_Counter();
    Counter* c2 = spawn_Counter();
    
    send_Counter(c1, 1, 0);
    send_Counter(c1, 1, 0);
    send_Counter(c2, 1, 0);
    
    Counter_step(c1);
    Counter_step(c1);
    Counter_step(c2);
    
    printf("Counter 1: %d\n", c1->count);
    printf("Counter 2: %d\n", c2->count);
    
    free(c1);
    free(c2);
    return 0;
}
