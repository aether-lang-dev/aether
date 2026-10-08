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
#include "../actors/aether_send_message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>  // Sleep() in scheduler_wait
#else
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
extern AETHER_TLS int g_inline_step_depth;
extern AETHER_TLS int g_inline_release_pending;

// ============================================================================
// Released actors (#2509)
// ============================================================================
//
// One thread, but a step can still release its own actor, and whatever ran
// that step (aether_scheduler_poll, scheduler_wait, an inline send) touches
// the actor after it returns. So an actor released while any of them is in
// progress is kept here and freed when the last one is done.

static int g_coop_walk_depth = 0;  // aether_scheduler_poll / scheduler_wait walks in progress
static ActorBase** g_coop_released = NULL;
static int g_coop_released_count = 0;
static int g_coop_released_capacity = 0;

// A released actor's block is kept, marked released, for the next spawn of
// its size (#2517): a send takes a raw pointer and reads `dead` first, so a
// late send is dropped rather than reading freed memory. See the threaded
// scheduler's notes. The blocks go back to the allocator in
// scheduler_cleanup.
static ActorBase** g_coop_kept = NULL;
static int g_coop_kept_count = 0;
static int g_coop_kept_capacity = 0;

static void coop_free_actor(ActorBase* actor) {
    // Anything still in the mailbox was sent against the release contract.
    Message discard;
    while (mailbox_receive(&actor->mailbox, &discard)) {
        if (discard.payload_ptr) aether_free_message(discard.payload_ptr);
        if (discard.zerocopy.owned && discard.zerocopy.data) free(discard.zerocopy.data);
    }
    if (actor->spsc_queue) {
        free(actor->spsc_queue);
        actor->spsc_queue = NULL;
    }
    // A caller's actor (scheduler_register_actor) is the caller's to free.
    if (!actor->scheduler_owned) return;
    if (g_coop_kept_count == g_coop_kept_capacity) {
        int cap = g_coop_kept_capacity ? g_coop_kept_capacity * 2 : 16;
        ActorBase** grown = realloc(g_coop_kept, (size_t)cap * sizeof(ActorBase*));
        if (grown) {
            g_coop_kept = grown;
            g_coop_kept_capacity = cap;
        }
    }
    if (g_coop_kept_count < g_coop_kept_capacity) {
        g_coop_kept[g_coop_kept_count++] = actor;
        return;
    }
    // Out of memory to keep it: the block goes back.
    aether_numa_free_aligned(actor, actor->alloc_size);
}

// The most recently kept block of this size, or NULL.
static ActorBase* coop_take_kept(size_t size) {
    for (int i = g_coop_kept_count - 1; i >= 0; i--) {
        if (g_coop_kept[i]->alloc_size == size) {
            ActorBase* actor = g_coop_kept[i];
            g_coop_kept[i] = g_coop_kept[--g_coop_kept_count];
            return actor;
        }
    }
    return NULL;
}

static void coop_free_kept(void) {
    for (int i = 0; i < g_coop_kept_count; i++) {
        aether_numa_free_aligned(g_coop_kept[i], g_coop_kept[i]->alloc_size);
    }
    free(g_coop_kept);
    g_coop_kept = NULL;
    g_coop_kept_count = 0;
    g_coop_kept_capacity = 0;
}

// Sends dropped because their target was dead; a released target is a
// send against the release contract, counted and reported once (#2517).
static uint64_t g_coop_released_sends = 0;
static int g_coop_released_send_reported = 0;

uint64_t scheduler_released_sends(void) {
    return g_coop_released_sends;
}

static int coop_send_to_dead_actor(ActorBase* actor, Message* msg) {
    int dead = actor ? atomic_load_explicit(&actor->dead, memory_order_relaxed) : 1;
    if (!dead) return 0;
    if (msg->payload_ptr) aether_free_message(msg->payload_ptr);
    if (msg->zerocopy.owned && msg->zerocopy.data) free(msg->zerocopy.data);
    if (dead & AETHER_ACTOR_RELEASED) {
        g_coop_released_sends++;
        if (!g_coop_released_send_reported) {
            g_coop_released_send_reported = 1;
            fprintf(stderr, "aether: message type %d sent to actor %d after its release; "
                            "dropped (reported once)\n", msg->type, actor->id);
        }
    }
    return 1;
}

// One thread: nothing but aether_scheduler_poll and scheduler_wait steps an
// actor, and their walk depth covers them.
void scheduler_reader_online(void) {}
void scheduler_reader_quiescent(void) {}
void scheduler_reader_offline(void) {}
void scheduler_actor_thread_exit(ActorBase* actor) { (void)actor; }

// Frees the released actors once no walk and no inline send is running.
static void coop_free_released(void) {
    if (g_coop_walk_depth > 0 || g_inline_step_depth > 0) return;
    while (g_coop_released_count > 0) {
        coop_free_actor(g_coop_released[--g_coop_released_count]);
    }
    free(g_coop_released);
    g_coop_released = NULL;
    g_coop_released_capacity = 0;
}

static int coop_defer_release(ActorBase* actor) {
    if (g_coop_released_count == g_coop_released_capacity) {
        int cap = g_coop_released_capacity ? g_coop_released_capacity * 2 : 4;
        ActorBase** grown = realloc(g_coop_released, (size_t)cap * sizeof(ActorBase*));
        if (!grown) return 0;
        g_coop_released = grown;
        g_coop_released_capacity = cap;
    }
    g_coop_released[g_coop_released_count++] = actor;
    return 1;
}

int scheduler_released_actors_pending(void) {
    return g_coop_released_count;
}

void scheduler_inline_step_done(void) {
    g_inline_release_pending = 0;
    coop_free_released();  // waits for an enclosing walk, which frees them at its end
}

// ============================================================================
// Initialization
// ============================================================================

// Set by scheduler_init(), cleared by scheduler_cleanup(). A library's actors
// in a program without actors of its own initialize the scheduler on their
// first spawn (#2297), and a second scheduler_init() must not wipe the table
// of the actors already registered.
static int g_coop_initialized = 0;

void scheduler_init(int cores) {
    (void)cores;
    if (g_coop_initialized) return;
    g_coop_initialized = 1;
    num_cores = 1;

    aether_detect_hardware();
    aether_init_from_env();

    // Initialize the single scheduler slot
    memset(&schedulers[0], 0, sizeof(Scheduler));
    schedulers[0].core_id = 0;
    // calloc: every slot starts NULL. One thread, so the table never grows
    // or retires (#2486 concerns the threaded scheduler).
    size_t table_size = sizeof(AetherActorTable) +
                        MAX_ACTORS_PER_CORE * sizeof(_Atomic(ActorBase*));
    AetherActorTable* table = calloc(1, table_size);
    if (!table) {
        // The scheduler cannot run any actor without this table; leaving it NULL
        // would crash scheduler_register_actor on the first spawn. Fail loudly
        // at init instead of a delayed NULL deref.
        fprintf(stderr, "aether: failed to allocate cooperative scheduler actor table\n");
        abort();
    }
    table->alloc_size = table_size;
    table->capacity = MAX_ACTORS_PER_CORE;
    atomic_store_explicit(&schedulers[0].actor_table, table, memory_order_relaxed);
    atomic_store_explicit(&schedulers[0].actor_count, 0, memory_order_relaxed);

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

void scheduler_start(void) {
    // No threads to start
}

void scheduler_ensure_threads_running(void) {
    // No threads in cooperative mode — this is a no-op
}

void scheduler_stop(void) {
    // No threads to stop
}

void scheduler_cleanup(void) {
    // Nothing runs a step any more.
    g_coop_walk_depth = 0;
    while (g_coop_released_count > 0) {
        coop_free_actor(g_coop_released[--g_coop_released_count]);
    }
    free(g_coop_released);
    g_coop_released = NULL;
    g_coop_released_capacity = 0;
    // Actors never released are reachable only through the table freed
    // below: they end the way a release ends them.
    {
        AetherActorTable* table = atomic_load_explicit(&schedulers[0].actor_table, memory_order_relaxed);
        int count = atomic_load_explicit(&schedulers[0].actor_count, memory_order_relaxed);
        for (int k = 0; table && k < count; k++) {
            ActorBase* actor = atomic_load_explicit(&table->slots[k], memory_order_relaxed);
            if (actor && actor->scheduler_owned) coop_free_actor(actor);
        }
    }
    coop_free_kept();
    free(atomic_load_explicit(&schedulers[0].actor_table, memory_order_relaxed));
    atomic_store_explicit(&schedulers[0].actor_table, NULL, memory_order_relaxed);
    atomic_store_explicit(&schedulers[0].actor_count, 0, memory_order_relaxed);
    g_coop_initialized = 0;
}

void scheduler_shutdown(void) {
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

static int register_actor(ActorBase* actor, int preferred_core);

// An actor the caller allocated: the scheduler runs it and never frees it.
int scheduler_register_actor(ActorBase* actor, int preferred_core) {
    actor->scheduler_owned = 0;
    return register_actor(actor, preferred_core);
}

static int register_actor(ActorBase* actor, int preferred_core) {
    (void)preferred_core;
    Scheduler* sched = &schedulers[0];
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);

    if (count >= table->capacity) {
        fprintf(stderr, "aether: cooperative scheduler: too many actors (%d)\n", count);
        return -1;
    }

    atomic_store_explicit(&table->slots[count], actor, memory_order_relaxed);
    atomic_store_explicit(&sched->actor_count, count + 1, memory_order_relaxed);
    atomic_store_explicit(&actor->assigned_core, 0, memory_order_relaxed);
    return 0;
}

ActorBase* scheduler_spawn_actor(int preferred_core, void (*step)(void*), size_t actor_size) {
    (void)preferred_core;
    scheduler_init(1);  // no-op once initialized; see g_coop_initialized
    if (actor_size < sizeof(ActorBase)) actor_size = sizeof(ActorBase);

    // A released actor's block of this size first (#2517). Generated actor
    // structs are aligned(64), which calloc does not honour (#2485); the
    // block goes back through the matching function in scheduler_cleanup.
    ActorBase* actor = coop_take_kept(actor_size);
    if (!actor) actor = aether_numa_alloc_aligned(actor_size, AETHER_ACTOR_ALIGN, -1);
    if (!actor) return NULL;
    memset(actor, 0, actor_size);

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

    actor->scheduler_owned = 1;
    register_actor(actor, 0);
    return actor;
}

void scheduler_release_actor(ActorBase* actor) {
    if (!actor) return;

    atomic_store_explicit(&actor->dead, AETHER_ACTOR_RELEASED, memory_order_relaxed);

    // Remove from scheduler's actor list, clearing the vacated slot as the
    // threaded scheduler does.
    Scheduler* sched = &schedulers[0];
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        if (atomic_load_explicit(&table->slots[i], memory_order_relaxed) == actor) {
            atomic_store_explicit(&table->slots[i],
                                  atomic_load_explicit(&table->slots[count - 1], memory_order_relaxed),
                                  memory_order_relaxed);
            atomic_store_explicit(&table->slots[count - 1], NULL, memory_order_relaxed);
            atomic_store_explicit(&sched->actor_count, count - 1, memory_order_relaxed);
            break;
        }
    }

    aether_on_actor_terminate();

    // A step may be releasing its own actor: free it once whatever runs the
    // step is done with it (#2509).
    if (g_coop_walk_depth > 0 || g_inline_step_depth > 0) {
        if (coop_defer_release(actor)) {
            if (g_inline_step_depth > 0) g_inline_release_pending = 1;
            return;
        }
        // Out of memory to remember it: keep it rather than free it under
        // the step.
        fprintf(stderr, "aether: out of memory deferring the release of actor %d; "
                        "it is not freed\n", actor->id);
        return;
    }
    coop_free_actor(actor);
}

// ============================================================================
// Message sending — cooperative: direct mailbox send, process via poll
// ============================================================================

void scheduler_send_local(ActorBase* actor, Message msg) {
    if (coop_send_to_dead_actor(actor, &msg)) return;
    mailbox_send(&actor->mailbox, msg);
}

void scheduler_send_remote(ActorBase* actor, Message msg, int from_core) {
    (void)from_core;
    // In cooperative mode there's only one core — all sends are local
    if (coop_send_to_dead_actor(actor, &msg)) return;
    mailbox_send(&actor->mailbox, msg);
}

// Batch send — trivial in single-threaded mode
void scheduler_send_batch_start(void) {
    // No-op
}

void scheduler_send_batch_add(ActorBase* actor, Message msg) {
    if (coop_send_to_dead_actor(actor, &msg)) return;
    mailbox_send(&actor->mailbox, msg);
}

void scheduler_send_batch_flush(void) {
    // No-op
}

// ============================================================================
// Wait for quiescence — poll until no messages remain
// ============================================================================

void scheduler_wait(void) {
    // Drain all pending messages cooperatively
    // Also wait for active timeouts to fire
    int idle_rounds = 0;
    while (idle_rounds < 100) {
        int processed = aether_scheduler_poll(0);
        if (processed == 0) {
            // Check if any actor has a pending timeout
            int has_pending_timeout = 0;
            g_coop_walk_depth++;
            Scheduler* sched = &schedulers[0];
            AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
            int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);
            for (int i = 0; i < count; i++) {
                ActorBase* a = atomic_load_explicit(&table->slots[i], memory_order_relaxed);
                if (a && a->timeout_ns > 0) {
                    has_pending_timeout = 1;
                    break;
                }
            }
            g_coop_walk_depth--;
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
    g_coop_walk_depth++;
    Scheduler* sched = &schedulers[0];
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);

    // The count is re-read every iteration: a step may spawn an actor.
    for (int i = 0; i < atomic_load_explicit(&sched->actor_count, memory_order_relaxed); i++) {
        ActorBase* actor = atomic_load_explicit(&table->slots[i], memory_order_relaxed);
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

    if (--g_coop_walk_depth == 0) coop_free_released();
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


/* See multicore_scheduler.h: generated code reaches these through calls, never
 * by naming the thread-local, so the TLS model stays inside this archive. */
int  aether_core_id_get(void)            { return current_core_id; }
void aether_core_id_set(int core_id)     { current_core_id = core_id; }
void aether_reply_slot_set(void* slot)   { g_current_reply_slot = slot; }
void aether_step_actor_set(void* actor)  { g_current_step_actor = actor; }
