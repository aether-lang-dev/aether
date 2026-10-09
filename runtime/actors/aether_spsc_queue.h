#ifndef AETHER_SPSC_QUEUE_H
#define AETHER_SPSC_QUEUE_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "actor_state_machine.h"

/**
 * Lock-Free SPSC (Single Producer Single Consumer) Queue
 * 
 * Optimized for same-core messaging where one actor sends to another
 * on the same scheduler thread. Uses only relaxed atomics since
 * there's no cross-core contention.
 * 
 * Benefits over mailbox with locks:
 * - No atomic RMW operations (test-and-set, etc.)
 * - No cache line bouncing
 * - Direct memory access with compiler barriers
 * 
 * Target: same-core messaging (one producer, one consumer) — avoids
 * the atomic RMW overhead of the general MPMC queue.
 */

// 64 slots: sufficient for same-core auto_process actor threads while keeping
// the embedded SPSC queue small enough for million-scale actor workloads.
// (The SPSC is unused for normal actors; only auto_process = 1 actors use it.)
#define SPSC_QUEUE_SIZE 64
#define SPSC_QUEUE_MASK (SPSC_QUEUE_SIZE - 1)

typedef struct {
    Message buffer[SPSC_QUEUE_SIZE];
    atomic_uint head;
    atomic_uint tail;
    char padding[56];  // Cache line alignment
} SPSCQueue;

// Initialize SPSC queue
static inline void spsc_queue_init(SPSCQueue* q) {
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
}

// Producer: enqueue single message (relaxed, no cross-core sync needed)
static inline int spsc_enqueue(SPSCQueue* q, Message msg) {
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t next_tail = (tail + 1) & SPSC_QUEUE_MASK;
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    
    if (next_tail == head) {
        return 0;  // Full
    }
    
    q->buffer[tail] = msg;
    atomic_store_explicit(&q->tail, next_tail, memory_order_release);
    return 1;
}

// Producer: enqueue batch of messages (single atomic update)
static inline int spsc_enqueue_batch(SPSCQueue* q, const Message* msgs, int count) {
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    
    // Calculate available space
    uint32_t space;
    if (head > tail) {
        space = head - tail - 1;
    } else {
        space = SPSC_QUEUE_SIZE - (tail - head) - 1;
    }
    
    if (space < (uint32_t)count) {
        return 0;  // Not enough space
    }
    
    // Copy messages
    for (int i = 0; i < count; i++) {
        q->buffer[tail] = msgs[i];
        tail = (tail + 1) & SPSC_QUEUE_MASK;
    }
    
    atomic_store_explicit(&q->tail, tail, memory_order_release);
    return count;
}

// Consumer: dequeue single message
static inline int spsc_dequeue(SPSCQueue* q, Message* out_msg) {
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    
    if (head == tail) {
        return 0;  // Empty
    }
    
    *out_msg = q->buffer[head];
    atomic_store_explicit(&q->head, (head + 1) & SPSC_QUEUE_MASK, memory_order_release);
    return 1;
}

// Consumer: dequeue batch of messages
static inline int spsc_dequeue_batch(SPSCQueue* q, Message* out_msgs, int max_count) {
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    
    if (head == tail) {
        return 0;  // Empty
    }
    
    // Calculate available messages
    uint32_t available;
    if (tail > head) {
        available = tail - head;
    } else {
        available = SPSC_QUEUE_SIZE - (head - tail);
    }
    
    uint32_t to_dequeue = (available < (uint32_t)max_count) ? available : (uint32_t)max_count;

    for (uint32_t i = 0; i < to_dequeue; i++) {
        out_msgs[i] = q->buffer[head];
        head = (head + 1) & SPSC_QUEUE_MASK;
    }
    
    atomic_store_explicit(&q->head, head, memory_order_release);
    return to_dequeue;
}

// Get approximate count (for monitoring)
static inline int spsc_count(SPSCQueue* q) {
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    
    if (tail >= head) {
        return tail - head;
    } else {
        return SPSC_QUEUE_SIZE - (head - tail);
    }
}

#endif // AETHER_SPSC_QUEUE_H
