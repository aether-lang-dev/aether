// Aether Cooperative Scheduler — Single-threaded backend
//
// Compiled INSTEAD of multicore_scheduler.c when AETHER_HAS_THREADS == 0.
// Implements the exact same API (multicore_scheduler.h) but runs everything
// on a single thread with cooperative message processing.
//
// This enables Aether programs to run on platforms without pthreads:
// WebAssembly (Emscripten), embedded systems, bare-metal, etc.

#include "multicore_scheduler.h"
#include "../config/aether_optimization_config.h"
#include "../aether_numa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// ============================================================================
// Globals — same names as multicore_scheduler.c so generated code links
// ============================================================================

Scheduler schedulers[MAX_CORES];  // Only [0] is used
int num_cores = 1;
atomic_int next_actor_id = 0;
AETHER_TLS int current_core_id = 0;  // Always core 0
AETHER_TLS void* g_pending_reply_slot = NULL;
AETHER_TLS void* g_current_reply_slot = NULL;
AETHER_TLS void* g_current_step_actor = NULL;

// Defined in aether_send_message.c
extern AETHER_TLS ActorBase* g_sync_step_actor;

// ============================================================================
// Initialization
// ============================================================================

void scheduler_init(int cores) {
    (void)cores;
    num_cores = 1;

    aether_detect_hardware();
    aether_init_from_env();

    // Initialize the single scheduler slot
    memset(&schedulers[0], 0, sizeof(Scheduler));
    schedulers[0].core_id = 0;
    schedulers[0].actors = calloc(MAX_ACTORS_PER_CORE, sizeof(ActorBase*));
    if (!schedulers[0].actors) {
        // The scheduler cannot run any actor without this table; leaving it NULL
        // with a non-zero capacity would crash scheduler_register_actor on the
        // first spawn. Fail loudly at init instead of a delayed NULL deref.
        fprintf(stderr, "aether: failed to allocate cooperative scheduler actor table\n");
        abort();
    }
    schedulers[0].actor_count = 0;
    schedulers[0].capacity = MAX_ACTORS_PER_CORE;

    // Force main-thread mode — all processing is cooperative
    atomic_store(&g_aether_config.main_thread_mode, true);
}

void scheduler_init_with_opts(int cores, AetherOptFlags opts) {
    scheduler_init(cores);
    aether_runtime_configure(opts);
}

// ============================================================================
// Lifecycle — mostly no-ops in cooperative mode
// ============================================================================

void scheduler_start() {
    // No threads to start
}

void scheduler_ensure_threads_running() {
    // No threads in cooperative mode — this is a no-op
}

void scheduler_stop() {
    // No threads to stop
}

void scheduler_cleanup() {
    if (schedulers[0].actors) {
        free(schedulers[0].actors);
        schedulers[0].actors = NULL;
    }
    schedulers[0].actor_count = 0;
}

void scheduler_shutdown() {
    // Drain all remaining messages
    int drained;
    do {
        drained = aether_scheduler_poll(0);
    } while (drained > 0);

    scheduler_stop();
    scheduler_cleanup();
}

// ============================================================================
// Actor registration and spawning
// ============================================================================

int scheduler_register_actor(ActorBase* actor, int preferred_core) {
    (void)preferred_core;
    Scheduler* sched = &schedulers[0];

    if (sched->actor_count >= sched->capacity) {
        fprintf(stderr, "aether: cooperative scheduler: too many actors (%d)\n", sched->actor_count);
        return -1;
    }

    sched->actors[sched->actor_count++] = actor;
    atomic_store_explicit(&actor->assigned_core, 0, memory_order_relaxed);
    return 0;
}

ActorBase* scheduler_spawn_actor(int preferred_core, void (*step)(void*), size_t actor_size) {
    (void)preferred_core;
    if (actor_size < sizeof(ActorBase)) actor_size = sizeof(ActorBase);

    ActorBase* actor = calloc(1, actor_size);
    if (!actor) return NULL;

    mailbox_init(&actor->mailbox);
    AETHER_STAT_INC(actors_malloced);

    actor->alloc_size = actor_size;
    actor->id = atomic_fetch_add(&next_actor_id, 1);
    actor->step = step;
    atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
    actor->auto_process = 0;
    actor->spsc_queue = NULL;
    atomic_store_explicit(&actor->assigned_core, 0, memory_order_relaxed);
    atomic_store_explicit(&actor->migrate_to, -1, memory_order_relaxed);
    atomic_store_explicit(&actor->main_thread_only, 1, memory_order_relaxed);  // Always main-thread
    atomic_store_explicit(&actor->reply_slot, NULL, memory_order_relaxed);
    atomic_flag_clear_explicit(&actor->step_lock, memory_order_relaxed);
    actor->timeout_ns = 0;
    actor->last_activity_ns = 0;

    // Track actor count for inline mode
    int prev_count = atomic_load_explicit(&g_aether_config.actor_count, memory_order_relaxed);
    aether_on_actor_spawn();

    if (prev_count == 0) {
        aether_enable_main_thread_mode(actor);
    }
    // In cooperative mode, main_thread_mode stays on even with multiple actors.
    // We force it back on because aether_on_actor_spawn() disables it for count > 1.
    atomic_store_explicit(&g_aether_config.main_thread_mode, true, memory_order_relaxed);

    scheduler_register_actor(actor, 0);
    return actor;
}

void scheduler_release_actor(ActorBase* actor) {
    if (!actor) return;

    // Remove from scheduler's actor list
    Scheduler* sched = &schedulers[0];
    for (int i = 0; i < sched->actor_count; i++) {
        if (sched->actors[i] == actor) {
            sched->actors[i] = sched->actors[sched->actor_count - 1];
            sched->actor_count--;
            break;
        }
    }

    aether_on_actor_terminate();
    free(actor);
}

// ============================================================================
// Message sending — cooperative: direct mailbox send, process via poll
// ============================================================================

void scheduler_send_local(ActorBase* actor, Message msg) {
    mailbox_send(&actor->mailbox, msg);
}

void scheduler_send_remote(ActorBase* actor, Message msg, int from_core) {
    (void)from_core;
    // In cooperative mode there's only one core — all sends are local
    mailbox_send(&actor->mailbox, msg);
}

// Batch send — trivial in single-threaded mode
void scheduler_send_batch_start(void) {
    // No-op
}

void scheduler_send_batch_add(ActorBase* actor, Message msg) {
    mailbox_send(&actor->mailbox, msg);
}

void scheduler_send_batch_flush(void) {
    // No-op
}

// ============================================================================
// Wait for quiescence — poll until no messages remain
// ============================================================================

void scheduler_wait() {
    // Drain all pending messages cooperatively
    // Also wait for active timeouts to fire
    int idle_rounds = 0;
    while (idle_rounds < 100) {
        int processed = aether_scheduler_poll(0);
        if (processed == 0) {
            // Check if any actor has a pending timeout
            int has_pending_timeout = 0;
            Scheduler* sched = &schedulers[0];
            for (int i = 0; i < sched->actor_count; i++) {
                ActorBase* a = sched->actors[i];
                if (a && a->timeout_ns > 0) {
                    has_pending_timeout = 1;
                    break;
                }
            }
            if (!has_pending_timeout) break;
            // Sleep briefly to avoid spinning while waiting for timeout
            #ifdef _WIN32
            Sleep(1);
            #elif defined(__EMSCRIPTEN__)
            // No sleep in WASM — just spin
            #else
            usleep(1000);  // 1ms
            #endif
            idle_rounds++;
        } else {
            idle_rounds = 0;  // Reset on progress
        }
    }
}

// ============================================================================
// Poll — process pending messages for all actors (cooperative dispatch)
// ============================================================================

int aether_scheduler_poll(int max_per_actor) {
    int total = 0;
    int limit = (max_per_actor <= 0) ? 1024 : max_per_actor;
    Scheduler* sched = &schedulers[0];

    for (int i = 0; i < sched->actor_count; i++) {
        ActorBase* actor = sched->actors[i];
        if (!actor || !actor->step) continue;

        int processed = 0;
        int has_messages = atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0;
        int has_timeout = actor->timeout_ns > 0 && actor->last_activity_ns > 0;

        if (has_messages) {
            while (processed < limit) {
                if (atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) == 0) break;
                actor->step(actor);
                processed++;
            }
        } else if (has_timeout) {
            // No messages but timeout is ticking — call step to check timeout
            actor->step(actor);
            processed++;
        }
        total += processed;
    }

    return total;
}

// ============================================================================
// Ask/Reply — simplified synchronous version
// ============================================================================

void* scheduler_ask_message(ActorBase* target, void* msg_data, size_t msg_size, int timeout_ms) {
    (void)timeout_ms;

    // Allocate reply slot on stack (single-threaded, no race)
    ActorReplySlot slot = {0};
    slot.reply_data = NULL;
    slot.reply_size = 0;
    slot.reply_ready = 0;
    slot.timed_out = 0;
    atomic_store(&slot.refcount, 1);

    // Set pending reply slot
    g_pending_reply_slot = &slot;

    // Send the message (will be processed synchronously if main_thread_mode)
    void* msg_copy = malloc(msg_size);
    if (!msg_copy) return NULL;
    memcpy(msg_copy, msg_data, msg_size);

    Message msg;
    msg.type = *(int*)msg_data;
    msg.sender_id = 0;
    msg.payload_int = 0;
    msg.payload_ptr = msg_copy;
    msg.zerocopy.data = NULL;
    msg.zerocopy.size = 0;
    msg.zerocopy.owned = 0;
    msg._reply_slot = &slot;

    mailbox_send(&target->mailbox, msg);
    g_pending_reply_slot = NULL;

    // Process messages until reply is ready
    int max_rounds = 100000;
    while (!slot.reply_ready && max_rounds-- > 0) {
        aether_scheduler_poll(1);
    }

    return slot.reply_data;
}

void scheduler_reply(ActorBase* self, void* data, size_t data_size) {
    (void)self;
    ActorReplySlot* slot = (ActorReplySlot*)g_current_reply_slot;
    if (!slot) return;

    if (slot->timed_out) return;

    slot->reply_data = malloc(data_size);
    if (slot->reply_data) {
        memcpy(slot->reply_data, data, data_size);
        slot->reply_size = data_size;
    }
    slot->reply_ready = 1;
}

// ============================================================================
// I/O event integration — no-ops in cooperative mode
// WASM and embedded targets have no kernel I/O poller.
// ============================================================================

int scheduler_io_register(int core_id, int fd, void* actor, uint32_t events) {
    (void)core_id; (void)fd; (void)actor; (void)events;
    return -1;  // I/O registration not supported in cooperative mode
}

void scheduler_io_unregister(int core_id, int fd) {
    (void)core_id; (void)fd;
}
