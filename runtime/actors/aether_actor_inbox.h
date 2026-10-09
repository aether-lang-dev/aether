#ifndef AETHER_ACTOR_INBOX_H
#define AETHER_ACTOR_INBOX_H

#include <stdatomic.h>
#include <stdlib.h>
#include "actor_state_machine.h"
#include "../utils/aether_compiler.h"

/* The inbox of an actor that runs on a thread of its own (auto_process,
 * #2598). Every thread that sends to the actor puts the message here, and
 * only the actor's thread takes messages out, into its mailbox. Any number
 * of senders, one taker, under a spinlock held for a few stores.
 *
 * A ring holds the first AETHER_INBOX_RING messages. Past that they wait in
 * a list, and while anything waits there every new message goes behind it,
 * so nothing is dropped and each sender's messages come out in the order it
 * sent them. (The single-producer queue this replaces had several producers
 * in practice: the core, the core's other actor threads and the actor
 * itself. Two of them enqueueing at once wrote the same slot, and a full
 * queue dropped the message, though it had been counted as sent.)
 *
 * `count` is the number held, ring and list together. It changes only under
 * the lock and is read without it: by the taker to skip an empty inbox, and
 * by its idle park for the last look before sleeping. */
#define AETHER_INBOX_RING 64

typedef struct ActorInboxNode {
    struct ActorInboxNode* next;
    Message msg;
} ActorInboxNode;

typedef struct ActorInbox {
    atomic_flag lock;
    atomic_int count;
    int head;                  /* the ring's oldest slot */
    int ring_count;
    Message ring[AETHER_INBOX_RING];
    ActorInboxNode* first;     /* the list, oldest first */
    ActorInboxNode* last;
} ActorInbox;

static inline void actor_inbox_lock(ActorInbox* ib) {
    while (atomic_flag_test_and_set_explicit(&ib->lock, memory_order_acquire)) {
        AETHER_CPU_PAUSE();
    }
}

static inline void actor_inbox_unlock(ActorInbox* ib) {
    atomic_flag_clear_explicit(&ib->lock, memory_order_release);
}

static inline ActorInbox* actor_inbox_new(void) {
    ActorInbox* ib = (ActorInbox*)calloc(1, sizeof(ActorInbox));
    if (ib) atomic_flag_clear_explicit(&ib->lock, memory_order_relaxed);
    return ib;
}

/* Adds `msg` behind everything held. Returns 0 only when a list node could
 * not be allocated; the message is then not queued. */
static inline int actor_inbox_push(ActorInbox* ib, Message msg) {
    actor_inbox_lock(ib);
    if (!ib->first && ib->ring_count < AETHER_INBOX_RING) {
        ib->ring[(ib->head + ib->ring_count) % AETHER_INBOX_RING] = msg;
        ib->ring_count++;
        atomic_fetch_add_explicit(&ib->count, 1, memory_order_release);
        actor_inbox_unlock(ib);
        return 1;
    }
    actor_inbox_unlock(ib);

    /* Allocated outside the lock. Meanwhile the ring may have emptied and
     * another sender put a message there: that one is another sender's, and
     * this sender's own earlier messages are already ahead of both. */
    ActorInboxNode* node = (ActorInboxNode*)malloc(sizeof(ActorInboxNode));
    if (!node) return 0;
    node->next = NULL;
    node->msg = msg;
    actor_inbox_lock(ib);
    if (ib->last) ib->last->next = node;
    else ib->first = node;
    ib->last = node;
    atomic_fetch_add_explicit(&ib->count, 1, memory_order_release);
    actor_inbox_unlock(ib);
    return 1;
}

/* Moves up to `max` messages, oldest first, into `out`; returns how many.
 * The ring's messages are older than the list's: one goes in the ring only
 * while the list is empty. */
static inline int actor_inbox_take(ActorInbox* ib, Message* out, int max) {
    if (atomic_load_explicit(&ib->count, memory_order_acquire) == 0) return 0;
    ActorInboxNode* spent = NULL;
    int n = 0;
    actor_inbox_lock(ib);
    while (n < max && ib->ring_count > 0) {
        out[n++] = ib->ring[ib->head];
        ib->head = (ib->head + 1) % AETHER_INBOX_RING;
        ib->ring_count--;
    }
    while (n < max && ib->first) {
        ActorInboxNode* node = ib->first;
        ib->first = node->next;
        if (!ib->first) ib->last = NULL;
        out[n++] = node->msg;
        node->next = spent;
        spent = node;
    }
    atomic_fetch_sub_explicit(&ib->count, n, memory_order_relaxed);
    actor_inbox_unlock(ib);
    while (spent) {
        ActorInboxNode* next = spent->next;
        free(spent);
        spent = next;
    }
    return n;
}

#endif /* AETHER_ACTOR_INBOX_H */
