#include "aether_send_buffer.h"
#include "aether_spsc_queue.h"
#include "../scheduler/multicore_scheduler.h"
#include "../scheduler/lockfree_queue.h"
#include <string.h>

// Thread-local send buffer
AETHER_TLS SendBuffer g_send_buffer = {NULL, {{0}}, 0, -1};

// Forward declare to access schedulers
extern Scheduler schedulers[];

// Flush accumulated messages to target actor
void send_buffer_flush(void) {
    if (g_send_buffer.count == 0 || !g_send_buffer.target) {
        return;
    }
    
    ActorBase* actor = (ActorBase*)g_send_buffer.target;
    // An actor with its own thread takes every message in its inbox (#2598):
    // its SPSC queue and mailbox are not a sender's to write.
    if (actor->auto_process) {
        for (int i = 0; i < g_send_buffer.count; i++) {
            scheduler_actor_thread_deliver(actor, g_send_buffer.buffer[i]);
        }
        g_send_buffer.count = 0;
        return;
    }
    int target_core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
    
    // Fast path: same core, use lock-free SPSC queue
    if (g_send_buffer.core_id == target_core && g_send_buffer.core_id >= 0) {
        // Try SPSC queue first (lock-free, fastest path)
        // spsc_queue is lazy-allocated (NULL for regular actors); allocate if needed
        if (!actor->spsc_queue) {
            actor->spsc_queue = calloc(1, sizeof(SPSCQueue));
            if (actor->spsc_queue) spsc_queue_init(actor->spsc_queue);
        }
        int sent = actor->spsc_queue
            ? spsc_enqueue_batch(actor->spsc_queue, g_send_buffer.buffer, g_send_buffer.count) : 0;
        if (sent == g_send_buffer.count) {
            atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
            g_send_buffer.count = 0;
            return;
        }
        
        // SPSC queue full, fall back to mailbox
        sent = mailbox_send_batch(&actor->mailbox, g_send_buffer.buffer, g_send_buffer.count);
        if (sent == g_send_buffer.count) {
            atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
            g_send_buffer.count = 0;
            return;
        }
        
        // Partial send - move unsent messages to front
        int unsent = g_send_buffer.count - sent;
        memmove(g_send_buffer.buffer, g_send_buffer.buffer + sent, unsent * sizeof(Message));
        g_send_buffer.count = unsent;
        return;
    }
    
    // Slow path: cross-core batch enqueue (KEY OPTIMIZATION)
    // Batch all messages to same actor in ONE atomic operation
    void* actors[SEND_BUFFER_SIZE];
    for (int i = 0; i < g_send_buffer.count; i++) {
        actors[i] = actor;  // All messages to same actor
    }
    
    // Use the per-sender SPSC channel for this thread (SPSC invariant: only this thread writes here).
    int from_idx = (g_send_buffer.core_id >= 0 && g_send_buffer.core_id < MAX_CORES)
                   ? g_send_buffer.core_id : MAX_CORES;
    if (queue_enqueue_batch(&schedulers[target_core].from_queues[from_idx],
                            actors, g_send_buffer.buffer, g_send_buffer.count)) {
        // Success - update work count once for entire batch
        atomic_fetch_add(&schedulers[target_core].work_count, g_send_buffer.count);
        g_send_buffer.count = 0;
    } else {
        // Queue full - fall back to individual sends with backoff
        for (int i = 0; i < g_send_buffer.count; i++) {
            scheduler_send_remote(actor, g_send_buffer.buffer[i], g_send_buffer.core_id);
        }
        g_send_buffer.count = 0;
    }
}
