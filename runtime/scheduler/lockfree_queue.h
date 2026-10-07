#ifndef LOCKFREE_QUEUE_H
#define LOCKFREE_QUEUE_H

#include <stdatomic.h>
#include <stdlib.h>
#include "../actors/actor_state_machine.h"

// Per-direction queue size per sender→receiver SPSC channel.
// With MAX_CORES+1 channels per receiver, total capacity per core = 17×1024 = 17408.
// Overflow beyond this goes to TLS deferred buffers (no deadlock).
// 1024 balances throughput vs memory: each queue = 1024 × 56 = 57 KB.
#ifndef QUEUE_SIZE  // overridable with -D; must be a power of two
#define QUEUE_SIZE 1024  // Slots per sender→receiver SPSC channel
#endif
#define QUEUE_MASK (QUEUE_SIZE - 1)

typedef struct {
    void* actor;
    Message msg;
} QueueItem;

// Cache-line aligned to prevent false sharing between cores
typedef struct {
    atomic_int head;
    char padding1[60];  // Pad to 64 bytes
    atomic_int tail;
    char padding2[60];  // Pad to 64 bytes
    QueueItem items[QUEUE_SIZE];
} AETHER_ALIGNED(64) LockFreeQueue;

static inline void queue_init(LockFreeQueue* q) {
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
}

static inline int queue_enqueue(LockFreeQueue* q, void* actor, Message msg) {
    int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    int next_tail = (tail + 1) & QUEUE_MASK;  // Fast power-of-2 masking
    int head = atomic_load_explicit(&q->head, memory_order_acquire);
    
    if (unlikely(next_tail == head)) {  // Full is unlikely
        return 0;
    }
    
    q->items[tail].actor = actor;
    q->items[tail].msg = msg;
    atomic_store_explicit(&q->tail, next_tail, memory_order_release);
    return 1;
}

static inline int queue_dequeue(LockFreeQueue* q, void** actor, Message* msg) {
    int head = atomic_load_explicit(&q->head, memory_order_relaxed);
    int tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    
    if (unlikely(head == tail)) {  // Empty is unlikely
        return 0;
    }
    
    *actor = q->items[head].actor;
    *msg = q->items[head].msg;
    atomic_store_explicit(&q->head, (head + 1) & QUEUE_MASK, memory_order_release);  // Fast power-of-2 masking
    return 1;
}

// Batch enqueue - reduces atomic operations from N to 1
// Returns number of messages actually enqueued (partial enqueue if queue is near full)
static inline int queue_enqueue_batch(LockFreeQueue* q, void** actors, Message* msgs, int count) {
    int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    int head = atomic_load_explicit(&q->head, memory_order_acquire);

    int space = (head - tail - 1) & QUEUE_MASK;
    if (unlikely(space == 0)) return 0;

    // Enqueue as many as fit — partial batch is better than dropping the whole batch
    int to_enqueue = count < space ? count : space;

    for (int i = 0; i < to_enqueue; i++) {
        q->items[tail].actor = actors[i];
        q->items[tail].msg = msgs[i];
        tail = (tail + 1) & QUEUE_MASK;
    }

    // Single atomic update for entire batch
    atomic_store_explicit(&q->tail, tail, memory_order_release);
    return to_enqueue;
}

// Batch dequeue - reduces atomic operations from N to 1
static inline int queue_dequeue_batch(LockFreeQueue* q, void** actors, Message* msgs, int max_count) {
    int head = atomic_load_explicit(&q->head, memory_order_relaxed);
    int tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    int available = (tail - head) & QUEUE_MASK;
    if (unlikely(available == 0)) {
        return 0;  // Empty
    }

    int count = available < max_count ? available : max_count;

    // Copy batch from queue
    for (int i = 0; i < count; i++) {
        actors[i] = q->items[head].actor;
        msgs[i] = q->items[head].msg;
        head = (head + 1) & QUEUE_MASK;
    }

    // Single atomic update for entire batch
    atomic_store_explicit(&q->head, head, memory_order_release);
    return count;
}

// Get current queue size (approximate - may change between read of head/tail)
static inline int queue_size(LockFreeQueue* q) {
    int head = atomic_load_explicit(&q->head, memory_order_relaxed);
    int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    return (tail - head) & QUEUE_MASK;
}

#endif
