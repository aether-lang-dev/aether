#include "actor_state_machine.h"
#include "../scheduler/multicore_scheduler.h"
#include "../config/aether_optimization_config.h"
#include "../utils/aether_compiler.h"
#include <string.h>
#include <stdlib.h>
#include "../utils/aether_thread.h"

// Thread-local core ID for optimization
extern AETHER_TLS int current_core_id;

// ActorBase is defined in multicore_scheduler.h

// ==============================================================================
// TIER 1 OPTIMIZATION: Thread-Local Message Payload Pool
// ==============================================================================
// Instead of malloc/free for every message payload (one alloc + one free per
// send/receive cycle on the hot path), draw from a per-thread pool of small
// buffers. Eliminates the allocator as a bottleneck for small-message traffic.

#define MSG_PAYLOAD_POOL_SIZE 256      // 256 buffers per thread
#define MSG_PAYLOAD_MAX_SIZE 256       // Max pooled message size (Ping/Pong ~16 bytes)

typedef struct {
    char buffer[MSG_PAYLOAD_MAX_SIZE];
    int in_use;  // Thread-local: no atomics needed
} PooledPayload;

typedef struct {
    PooledPayload payloads[MSG_PAYLOAD_POOL_SIZE];
    int next_index;  // Thread-local: no atomics needed
    int initialized;
} PayloadPool;

// Thread-local payload pool
static AETHER_TLS PayloadPool* g_payload_pool = NULL;

// Global statistics (atomic, shared across all threads)
static _Atomic uint64_t g_pool_hits = 0;
static _Atomic uint64_t g_pool_misses = 0;
static _Atomic uint64_t g_too_large = 0;

// Get pool statistics (for debugging/profiling)
void aether_message_pool_stats(uint64_t* hits, uint64_t* misses, uint64_t* large) {
    *hits = atomic_load_explicit(&g_pool_hits, memory_order_relaxed);
    *misses = atomic_load_explicit(&g_pool_misses, memory_order_relaxed);
    *large = atomic_load_explicit(&g_too_large, memory_order_relaxed);
}

// Initialize thread-local payload pool
/* The acquire side of this pool was removed: it was unreachable, and the
 * send path below documents why it must stay that way. An actor can be
 * migrated to another core after a message is sent, so a TLS-pooled
 * payload would be released by a thread that does not own that pool.
 * payload_pool_release still runs and simply reports "not mine" for
 * every heap payload. */


// Return payload to pool
static inline int payload_pool_release(void* ptr) {
    if (!g_payload_pool || !g_payload_pool->initialized) {
        return 0;  // Not from pool
    }

    // Check if pointer is within pool memory
    char* pool_start = (char*)g_payload_pool->payloads;
    char* pool_end = pool_start + sizeof(g_payload_pool->payloads);

    if ((char*)ptr < pool_start || (char*)ptr >= pool_end) {
        return 0;  // Not from pool
    }

    // Find which slot this is
    size_t offset = (char*)ptr - pool_start;
    size_t slot_index = offset / sizeof(PooledPayload);

    if (slot_index >= MSG_PAYLOAD_POOL_SIZE) {
        return 0;  // Invalid
    }

    // Mark as free (thread-local: plain store)
    g_payload_pool->payloads[slot_index].in_use = 0;
    return 1;  // Successfully returned to pool
}

// TLS flag: when set, skip freeing (used for sync mode zero-copy)
extern AETHER_TLS int g_skip_free;

// Free message payload - try pool first, then fall back to free()
void aether_free_message(void* msg_data) {
    if (!msg_data) return;

    // SYNC MODE: Skip free when processing messages with zero-copy stack pointers
    if (unlikely(g_skip_free)) {
        return;  // Caller's stack memory - don't free!
    }

    // Try to return to pool
    if (payload_pool_release(msg_data)) {
        return;  // Returned to pool successfully
    }

    // Was malloc'd (large message or pool exhausted), use regular free
    free(msg_data);
}

// ==============================================================================
// Message Sending with Pool Optimization
// ==============================================================================

// ==============================================================================
// MAIN THREAD MODE: Synchronous Message Processing
// ==============================================================================
// When main thread mode is active:
// - Single actor, all sends from main(), no scheduler threads
// - Messages processed synchronously in sender's context
// - Zero queue overhead: 50M+ msg/sec possible
//
// This is the fastest path for single-actor programs like the counting benchmark.

// TLS flag: when set, aether_free_message skips freeing (used for sync mode)
// Declared here, accessed by aether_free_message below
AETHER_TLS int g_skip_free = 0;

// TLS pointer: the actor whose step() is currently being executed synchronously
// on the main thread (non-NULL only inside aether_send_message_sync).
// scheduler_spawn_actor checks this to defer main_thread_only=0 if the second
// actor is spawned while the first actor's step() is still on the call stack.
// Without the deferral a scheduler thread can enter the same actor's step()
// concurrently with the main thread → data race / crash.
AETHER_TLS ActorBase* g_sync_step_actor = NULL;

static inline void AETHER_HOT aether_send_message_sync(ActorBase* actor, void* message_data, size_t message_size) {
    Message msg;
    msg.type = *(int*)message_data;
    msg.sender_id = 0;
    msg.payload_int = 0;
    msg.zerocopy.data = NULL;
    msg.zerocopy.size = 0;
    msg.zerocopy.owned = 0;
    msg._reply_slot = g_pending_reply_slot;

#if AETHER_HAS_THREADS
    // ULTRA-FAST PATH: Pass original message directly without copying
    // Since processing is synchronous, caller's stack memory is still valid
    // We mark g_skip_free so step() doesn't try to free the caller's stack
    msg.payload_ptr = message_data;
#else
    // COOPERATIVE MODE: heap-allocate the message data.
    // The mailbox may have other messages queued ahead, so the immediate
    // step() call below may consume a different message (FIFO order).
    // The just-sent message will be processed later by scheduler_wait(),
    // by which time the caller's stack frame may be gone.
    void* heap_copy = malloc(message_size);
    if (!heap_copy) {
        // Delivering a NULL payload would crash the handler (or, for a
        // single-int message, silently run it on zeroed data). Match the
        // threaded path below and fail loudly rather than corrupt state.
        fprintf(stderr, "aether: malloc(%zu) failed for msg type %d to actor %d\n",
                message_size, *(int*)message_data, actor->id);
        abort();
    }
    memcpy(heap_copy, message_data, message_size);
    msg.payload_ptr = heap_copy;
#endif

    mailbox_send(&actor->mailbox, msg);

#if AETHER_HAS_THREADS
    // Tell aether_free_message to skip freeing the initial (stack-allocated) message.
    // Self-sent messages from handlers are heap-allocated (see g_sync_step_actor check
    // in aether_send_message), so they can be freed normally.
    g_skip_free = 1;
#endif
    // Guard: scheduler_spawn_actor defers main_thread_only=0 while this is set
    g_sync_step_actor = actor;
    actor->step(actor);
    // If step() triggered a self-send transition (which starts scheduler
    // threads and sets step_lock to prevent concurrent step() calls),
    // release the lock now that step() has returned safely.
    if (unlikely(!aether_main_thread_mode_active())) {
        atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
    }
#if AETHER_HAS_THREADS
    g_skip_free = 0;
#endif

    // Do NOT drain self-sent messages here.  If a handler does self ! Msg {},
    // the message sits in the mailbox and will be processed on the NEXT call to
    // aether_send_message (from main) or by the scheduler.  Draining eagerly
    // would turn self-continuation patterns (animation loops) into infinite
    // blocking loops that starve main().
    g_sync_step_actor = NULL;

    // Deferred main_thread_only disable: if the second actor was spawned during
    // step() above, scheduler_spawn_actor skipped clearing main_thread_only to
    // avoid a concurrent-step race.  Now that step() has returned it is safe.
    if (!aether_main_thread_mode_active() &&
        atomic_load_explicit(&actor->main_thread_only, memory_order_relaxed)) {
        atomic_store_explicit(&actor->main_thread_only, 0, memory_order_release);
    }

    // Track stats
    AETHER_STAT_INC(inline_sends);
}

// Send a typed message to an actor using optimized scheduler paths
// Uses SPSC queues, direct sends, and other optimizations automatically
void aether_send_message(void* actor_ptr, void* message_data, size_t message_size) {
    ActorBase* actor = (ActorBase*)actor_ptr;

    // ==============================================================================
    // FAST PATH: Main thread mode (single actor, synchronous processing)
    // ==============================================================================
    // For single-actor programs (like counting benchmark), process immediately.
    // No scheduler threads, no queues, no atomics - pure function call.
    if (aether_main_thread_mode_active()) {
#if AETHER_HAS_THREADS
        // If we're already inside a step() (self-send from a handler), the actor
        // needs to run independently from the main thread.  Disable main-thread
        // mode so the scheduler threads take over message processing.  This
        // enables patterns like animation loops (self ! AnimateStep {}) that
        // require the actor to process messages concurrently with main().
        if (g_sync_step_actor != NULL) {
            // Self-send from a handler: disable main-thread mode so the
            // scheduler takes over message processing for this actor.
            atomic_store_explicit(&g_aether_config.main_thread_mode, false, memory_order_release);
            g_aether_config.main_actor = NULL;
            atomic_store_explicit(&actor->main_thread_only, 0, memory_order_release);
            // Hold step_lock while the main thread's step() is still on the
            // call stack.  Without this, the scheduler threads (started below)
            // could call step() concurrently — a data race on actor state.
            // aether_send_message_sync releases the lock after step() returns.
            atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire);
            // Start scheduler threads if they were never created (main-thread
            // mode skips thread creation in scheduler_start()).
            scheduler_ensure_threads_running();
            // Fall through to the standard multi-actor send path below.
        } else
#endif // AETHER_HAS_THREADS
        {
            aether_send_message_sync(actor, message_data, message_size);
            return;
        }
    }

#if AETHER_HAS_THREADS
    // ==============================================================================
    // STANDARD PATH: Multi-actor scheduler-based processing
    // ==============================================================================
    // Always use heap allocation for type-safe messages.
    // TLS pools have thread affinity issues: a same-core actor may be migrated
    // to another core after the message is sent, causing the receiver to call
    // free() on pool memory (heap-use-after-free / free-on-non-malloc).
    void* msg_copy = malloc(message_size);
    if (!msg_copy) {
        fprintf(stderr, "aether: malloc(%zu) failed for msg type %d to actor %d\n",
                message_size, *(int*)message_data, actor->id);
        abort();
    }
    memcpy(msg_copy, message_data, message_size);

    Message msg;
    msg.type = *(int*)message_data;
    msg.sender_id = 0;
    msg.payload_int = 0;
    msg.payload_ptr = msg_copy;
    msg.zerocopy.data = NULL;
    msg.zerocopy.size = 0;
    msg.zerocopy.owned = 0;
    msg._reply_slot = g_pending_reply_slot;

    // Cache TLS once — avoids repeated tlv_get_addr calls on macOS
    const int my_core = current_core_id;

    if (my_core >= 0 && my_core == atomic_load_explicit(&actor->assigned_core, memory_order_relaxed)) {
        scheduler_send_local(actor, msg);
    } else {
        scheduler_send_remote(actor, msg, my_core);
    }
#else
    // No threads: always use synchronous processing
    aether_send_message_sync(actor, message_data, message_size);
#endif // AETHER_HAS_THREADS
}
