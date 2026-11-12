// Aether Cooperative Scheduler — Single-threaded backend
//
// Compiled INSTEAD of multicore_scheduler.c when AETHER_HAS_THREADS == 0.
// Implements the exact same API (multicore_scheduler.h) but runs everything
// on a single thread with cooperative message processing.
//
// This enables Aether programs to run on platforms without pthreads:
// WebAssembly (Emscripten), embedded systems, bare-metal, etc.

#include "multicore_scheduler.h"
#include "../utils/aether_fatal.h"   /* aether_fatal_flush (#3435) */
#include "../config/aether_optimization_config.h"
#include "../aether_numa.h"
#include "../actors/aether_send_message.h"
#include "../actors/aether_actor_inbox.h"
#include "../actors/aether_step_barrier.h"
#include "../aether_resource_caps.h"
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
AETHER_TLS void* g_current_reply_slot = NULL;
AETHER_TLS void* g_current_step_actor = NULL;
AETHER_TLS int aether_in_actor_thread = 0;   // set by aether_actor_thread.c (#3463)

// ============================================================================
// Released actors (#2509)
// ============================================================================
//
// One thread, but a step can still release its own actor, and whatever ran
// that step (aether_scheduler_poll, scheduler_wait) touches the actor after
// it returns. So an actor released while any of them is in
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
    // Anything still in the mailbox or waiting behind it was sent against the
    // release contract.
    Message discard;
    while (mailbox_receive(&actor->mailbox, &discard)) aether_message_drop(actor, &discard);
    ActorInbox* ib = atomic_exchange_explicit(&actor->inbox, NULL, memory_order_relaxed);
    if (ib) {
        Message spill[AETHER_INBOX_RING];
        int got;
        while ((got = actor_inbox_take(ib, spill, AETHER_INBOX_RING)) > 0) {
            for (int k = 0; k < got; k++) aether_message_drop(actor, &spill[k]);
        }
        free(ib);
    }
    aether_watch_free(actor);   // its death notices (#2974)
    if (actor->spsc_queue) {
        free(actor->spsc_queue);
        actor->spsc_queue = NULL;
    }
    // A caller's actor (scheduler_register_actor) is the caller's to free.
    if (!actor->scheduler_owned) return;
    // What the state owns goes before the block is kept (#2528).
    if (actor->destroy_state) {
        actor->destroy_state(actor);
        actor->destroy_state = NULL;
    }
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

/* The cooperative scheduler runs on its one thread: no other thread sends
 * to it, stopped or not (#2912). */
uint64_t scheduler_stopped_sends(void) {
    return 0;
}

AetherActorRef scheduler_actor_ref(ActorBase* actor) {
    AetherActorRef ref = { actor, actor ? actor->id : 0 };
    return ref;
}

void scheduler_send_ref(AetherActorRef ref, Message msg) {
    if (ref.actor && ref.actor->id == ref.id) {
        scheduler_send_remote(ref.actor, msg, -1);
        return;
    }
    // The block is another actor's now, whose message definitions need not
    // be the sender's: the payload alone is freed.
    aether_message_drop(NULL, &msg);
    g_coop_released_sends++;
    if (!g_coop_released_send_reported) {
        g_coop_released_send_reported = 1;
        fprintf(stderr, "aether: message type %d sent to actor %d after its release; "
                        "dropped (reported once)\n", msg.type, ref.id);
    }
}

static int coop_send_to_dead_actor(ActorBase* actor, Message* msg) {
    int dead = actor ? atomic_load_explicit(&actor->dead, memory_order_relaxed) : 1;
    if (!dead) return 0;
    aether_mailbox_untake(actor, msg->type);   // #2974
    aether_message_drop(actor, msg);
    /* An ask to it gets no reply: its asker learns so at once (#3151). */
    if (msg->_reply_slot) ((ActorReplySlot*)msg->_reply_slot)->failed = AETHER_ASK_DEAD;
    if (dead & AETHER_ACTOR_RELEASED) {
        g_coop_released_sends++;
        if (!g_coop_released_send_reported) {
            g_coop_released_send_reported = 1;
            fprintf(stderr, "aether: message type %d sent to actor %d after its release; "
                            "dropped (reported once)\n", msg->type, actor->id);
        }
    } else if (actor) {
        scheduler_count_dead_sends(1);   // panicked, killed or stopped (#2974)
    }
    return 1;
}

// One thread: nothing but aether_scheduler_poll and scheduler_wait steps an
// actor, and their walk depth covers them.

/* A send queues (#3112): into the mailbox while it has room and nothing waits
 * behind it, else into the actor's inbox (aether_actor_inbox.h, an unbounded
 * FIFO), from which coop_refill moves the messages on, oldest first, as the
 * mailbox empties. A send never runs a handler; aether_scheduler_poll and
 * wait_for_idle do. */
static void coop_deliver(ActorBase* actor, Message msg) {
    if (coop_send_to_dead_actor(actor, &msg)) return;
    ActorInbox* ib = atomic_load_explicit(&actor->inbox, memory_order_relaxed);
    if ((!ib || atomic_load_explicit(&ib->count, memory_order_relaxed) == 0) &&
        mailbox_send(&actor->mailbox, msg))
        return;
    if (!ib) {
        ib = actor_inbox_new();
        if (!ib) {
            aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
            fprintf(stderr, "aether: out of memory queueing message type %d for actor %d\n",
                    msg.type, actor->id);
            abort();
        }
        atomic_store_explicit(&actor->inbox, ib, memory_order_relaxed);
    }
    if (!actor_inbox_push(ib, msg)) {
        aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
        fprintf(stderr, "aether: out of memory queueing message type %d for actor %d\n",
                msg.type, actor->id);
        abort();
    }
}

/* A bounded mailbox (#2974), as the threaded scheduler has it: a send takes
 * its place first, or is refused and released, never blocked on. A refused
 * ask fails at once; a refused plain send is counted, reported once. */
static uint64_t g_coop_full_drops = 0;
static int g_coop_full_drop_reported = 0;

uint64_t scheduler_full_drops(void) {
    return g_coop_full_drops;
}

static void coop_send(ActorBase* actor, Message msg) {
    if (actor && atomic_load_explicit(&actor->mailbox_bound, memory_order_relaxed) &&
        !aether_mailbox_admit(actor, msg.type)) {
        aether_message_drop(actor, &msg);
        if (msg._reply_slot) {
            ((ActorReplySlot*)msg._reply_slot)->failed = AETHER_ASK_FULL;
            return;
        }
        atomic_fetch_add_explicit(&actor->full_drops, 1, memory_order_relaxed);
        g_coop_full_drops++;
        if (!g_coop_full_drop_reported) {
            g_coop_full_drop_reported = 1;
            fprintf(stderr, "aether: message type %d to actor %d dropped: its mailbox is full "
                            "(bound %d); counted by actors.full_drops (reported once)\n",
                    msg.type, actor->id,
                    atomic_load_explicit(&actor->mailbox_bound, memory_order_relaxed));
        }
        return;
    }
    coop_deliver(actor, msg);
}

const char* scheduler_try_send(ActorBase* actor, Message msg) {
    if (!actor || atomic_load_explicit(&actor->dead, memory_order_relaxed)) {
        aether_message_drop(actor, &msg);
        return "the actor is dead";
    }
    if (!aether_mailbox_admit(actor, msg.type)) {
        aether_message_drop(actor, &msg);
        return "the mailbox is full";
    }
    coop_deliver(actor, msg);
    return "";
}

// Moves what waits in the actor's inbox into its mailbox, as room allows.
static void coop_refill(ActorBase* actor) {
    ActorInbox* ib = atomic_load_explicit(&actor->inbox, memory_order_relaxed);
    if (!ib) return;
    int room = MAILBOX_SIZE - atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed);
    if (room <= 0) return;
    Message moved[MAILBOX_SIZE];
    int n = actor_inbox_take(ib, moved, room);
    for (int k = 0; k < n; k++) mailbox_send(&actor->mailbox, moved[k]);
}

static int coop_has_messages(ActorBase* actor) {
    ActorInbox* ib = atomic_load_explicit(&actor->inbox, memory_order_relaxed);
    return atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0 ||
           (ib && atomic_load_explicit(&ib->count, memory_order_relaxed) > 0);
}
void scheduler_reader_online(void) {}
void scheduler_reader_quiescent(void) {}
void scheduler_reader_offline(void) {}
void scheduler_actor_thread_exit(ActorBase* actor) { (void)actor; }
int scheduler_actor_thread_park(ActorBase* actor) { (void)actor; return 0; }
void scheduler_actor_thread_deliver(ActorBase* actor, Message msg) { (void)actor; (void)msg; }
void scheduler_set_actor_park_ms(int ms) { (void)ms; }
void scheduler_set_core_park_ms(int ms) { (void)ms; }

// Frees the released actors once no walk is running.
static void coop_free_released(void) {
    if (g_coop_walk_depth > 0) return;
    while (g_coop_released_count > 0) {
        coop_free_actor(g_coop_released[--g_coop_released_count]);
    }
}

// One thread: the released actors are ended here and now (#2528), unless a
// walk is on the stack, which ends them when it returns.
int scheduler_reclaim_released(void) {
    coop_free_released();
    return g_coop_released_count;
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

    // Before this copy's first step, as the threaded scheduler does (#3438):
    // a build without threads still runs the sandbox and the step barrier.
    void (*join)(void) = atomic_load_explicit(&aether_sandbox_join, memory_order_acquire);
    if (join) join();

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
        aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
        fprintf(stderr, "aether: failed to allocate cooperative scheduler actor table\n");
        abort();
    }
    table->alloc_size = table_size;
    table->capacity = MAX_ACTORS_PER_CORE;
    atomic_store_explicit(&schedulers[0].actor_table, table, memory_order_relaxed);
    atomic_store_explicit(&schedulers[0].actor_count, 0, memory_order_relaxed);
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
    actor->release_message = NULL;   // its messages are the caller's (#3142)
    actor->step_payload = NULL;
    atomic_store_explicit(&actor->death_watch, NULL, memory_order_relaxed);   // #2974
    atomic_store_explicit(&actor->exit_flags, 0, memory_order_relaxed);
    atomic_store_explicit(&actor->incarnation, 0, memory_order_relaxed);
    atomic_store_explicit(&actor->mailbox_bound, 0, memory_order_relaxed);   // #2974
    atomic_store_explicit(&actor->mailbox_pending, 0, memory_order_relaxed);
    atomic_store_explicit(&actor->full_drops, 0, memory_order_relaxed);
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

    // A restart in place (#2974): the child's own block.
    ActorBase* reborn = aether_rebirth_take(step, actor_size);
    if (reborn) return reborn;

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
    atomic_store_explicit(&actor->reply_slot, NULL, memory_order_relaxed);
    atomic_flag_clear_explicit(&actor->step_lock, memory_order_relaxed);
    actor->timeout_ns = 0;
    actor->last_activity_ns = 0;

    aether_on_actor_spawn();

    actor->scheduler_owned = 1;
    register_actor(actor, 0);
    return actor;
}

void scheduler_release_actor(ActorBase* actor) {
    if (!actor) return;

    // Its watchers are told (#2974): a release is a normal end unless
    // something ended it first, and one told already is told nothing more.
    if (!(atomic_load_explicit(&actor->dead, memory_order_relaxed) & AETHER_ACTOR_RELEASED)) {
        aether_actor_claim_reason(actor, "normal");
        aether_watch_notify(actor, aether_actor_death_reason(actor));
    }
    atomic_fetch_or_explicit(&actor->dead, AETHER_ACTOR_RELEASED, memory_order_relaxed);

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
    if (g_coop_walk_depth > 0) {
        if (coop_defer_release(actor)) return;
        // Out of memory to remember it: keep it rather than free it under
        // the step.
        fprintf(stderr, "aether: out of memory deferring the release of actor %d; "
                        "it is not freed\n", actor->id);
        return;
    }
    coop_free_actor(actor);
}

// ============================================================================
// Message sending — cooperative: queued (coop_deliver), processed via poll
// ============================================================================

void scheduler_send_local(ActorBase* actor, Message msg) {
    coop_send(actor, msg);
}

void scheduler_send_remote(ActorBase* actor, Message msg, int from_core) {
    (void)from_core;
    // In cooperative mode there's only one core — all sends are local
    coop_send(actor, msg);
}

void scheduler_send_many(ActorBase* actor, const Message* msgs, int n) {
    for (int i = 0; i < n; i++) coop_send(actor, msgs[i]);
}

// Batch send — trivial in single-threaded mode
void scheduler_send_batch_start(void) {
    // No-op
}

void scheduler_send_batch_add(ActorBase* actor, Message msg) {
    coop_send(actor, msg);
}

void scheduler_send_batch_flush(void) {
    // No-op
}

// ============================================================================
// Wait for quiescence — poll until no messages remain
// ============================================================================

/* Whether a live actor's receive timeout is armed (#3162). */
static int coop_timeout_armed(void) {
    int armed = 0;
    g_coop_walk_depth++;
    Scheduler* sched = &schedulers[0];
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        ActorBase* a = atomic_load_explicit(&table->slots[i], memory_order_relaxed);
        if (a && a->timeout_ns > 0 && a->last_activity_ns > 0 &&
            atomic_load_explicit(&a->dead, memory_order_relaxed) == 0) {
            armed = 1;
            break;
        }
    }
    g_coop_walk_depth--;
    return armed;
}

/* wait_for_idle(): every message handled, and no live actor's receive
 * timeout armed, as on the multicore scheduler (#3102): a countdown fires
 * once its deadline passes, and its arm may send more. While one counts
 * down the poll does no work, and the wait sleeps a millisecond at a time,
 * bounded as the multicore scheduler bounds it (100 s); it used to spin on
 * the CPU, the poll counting each check of the deadline as work. The end of
 * main does not wait for countdowns (scheduler_shutdown, #3219).
 *
 * Inside an actor's step, or on an actor's own thread, it panics, as on the
 * multicore scheduler (#3463): there it waits for a step that cannot end
 * while it waits; here the poll ran the other actors nested and returned,
 * and a handler's wait now behaves alike on both. A drop or a death hook
 * this scheduler runs after a step still polls nested and returns: the one
 * thread here steps every actor, and is in no step then. */
void scheduler_wait(void) {
    if (g_aether_in_actor_step || aether_in_actor_thread) aether_panic(AETHER_WAIT_IN_STEP_REASON);
    uint64_t give_up = aether_now_ns() + 100ULL * 1000000000ULL;
    for (;;) {
        if (aether_scheduler_poll(0) > 0) continue;
        if (!coop_timeout_armed() || aether_now_ns() >= give_up) break;
        // Sleep briefly while a timeout counts down
        #ifdef _WIN32
        Sleep(1);
        #elif defined(__EMSCRIPTEN__)
        // No sleep in WASM: the countdown is polled
        #else
        usleep(1000);  // 1ms
        #endif
    }
}

// ============================================================================
// Poll — process pending messages for all actors (cooperative dispatch)
// ============================================================================

/* One step with a step context of its own (multicore_scheduler.h). An ask
 * polls until its reply comes, so a step runs inside the asking one, as on a
 * core's inline path: it starts with no ask in hand, fails one its handler
 * left unanswered, and gives the enclosing step its context back. */
// ----------------------------------------------------------------------------
// The end of an actor (#2974), as the threaded scheduler's notes describe it:
// one thread, so its death is finalized where it is seen, unless the actor is
// stepping (its step lock taken by the outermost step on the stack), when its
// step's end finalizes it.
// ----------------------------------------------------------------------------

static uint64_t g_coop_dead_sends = 0;

uint64_t scheduler_dead_sends(void) {
    return g_coop_dead_sends;
}

void scheduler_count_dead_sends(int n) {
    if (n > 0) g_coop_dead_sends += (uint64_t)n;
}

/* What waits for it is dropped (an ask in it fails as dead), and its
 * watchers are told. Once. */
static void coop_finalize(ActorBase* actor) {
    int flags = atomic_load_explicit(&actor->exit_flags, memory_order_relaxed);
    if (flags & AETHER_EXIT_FINALIZED) return;
    atomic_store_explicit(&actor->exit_flags, flags | AETHER_EXIT_FINALIZED, memory_order_relaxed);
    int drained = 0;
    Message discard;
    for (;;) {
        coop_refill(actor);
        if (!mailbox_receive(&actor->mailbox, &discard)) break;
        aether_mailbox_untake(actor, discard.type);
        // A panic in its release is reported, and the end goes on (#3401).
        aether_message_drop_at_end(actor, &discard);
        if (discard._reply_slot) ((ActorReplySlot*)discard._reply_slot)->failed = AETHER_ASK_DEAD;
        drained++;
    }
    scheduler_count_dead_sends(drained);
    const char* reason = aether_actor_death_reason(actor);
    aether_watch_notify(actor, reason ? reason : "normal");
}

void scheduler_actor_thread_finalize(ActorBase* actor) {
    (void)actor;   // no actor has a thread of its own here
}

void scheduler_actor_revived(ActorBase* actor) {
    (void)actor;   // the next poll steps it
}

/* The cooperative poll looks at every actor's countdown itself (#3540 is
 * the threaded scheduler's ready list). */
void scheduler_timeout_armed(ActorBase* actor) {
    (void)actor;
}

/* No actor thread runs here (aether_actor_thread_start refuses without
 * threads); the call links, as the actor thread's loop does. */
void scheduler_count_processed(int n) {
    (void)n;
}

/* An ask the step leaves without a reply: its asker, polling, sees why. */
static void coop_fail_slot(void* slot, int why) {
    ActorReplySlot* s = (ActorReplySlot*)slot;
    if (!s->reply_ready && !s->failed) s->failed = why;
}

int scheduler_actor_end(ActorBase* actor, const char* reason, int bit) {
    if (!actor || !aether_actor_claim_end(actor, reason, bit)) return 0;
    if (!atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
        coop_finalize(actor);
        atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
    }
    return 1;
}

/* One step of `actor`, or none: 1 when it stepped. Its step lock is held
 * while it steps. An actor whose step is on the stack already (it asked, and
 * this is the ask's poll) is not stepped again inside it (#3391): its next
 * message waits for the handler to return, as with threads, where the
 * waiting thread steps only its target and an actor stepping is left alone.
 * A second step there ran the actor's next handler inside the first, its
 * state changing under it, and a stop() or a panic in it finalized the
 * actor (its Down sent, its queue dropped) while the first still ran. */
static int coop_step(ActorBase* actor) {
    if (!actor || !actor->step) return 0;
    if (atomic_load_explicit(&actor->dead, memory_order_relaxed)) return 0;
    if (atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) return 0;
    // Inside the panic barrier both schedulers share (#2974): a panic is a
    // death, finalized here, with its Down, not the end of the program; a
    // stop() or a kill that came while the handler ran is finalized after it.
    // Where the target has no setjmp (wasi) a panic still ends the program.
    aether_step_barrier(actor, coop_finalize, coop_fail_slot, NULL);
    atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
    return 1;
}

int scheduler_step_actor(ActorBase* actor) {
    return coop_step(actor);
}

/* A firing countdown is counted for wait_for_idle() on the multicore
 * scheduler (#3443). Here the thread that waits is the one that steps, so a
 * wait never returns while an arm runs: nothing to count. */
void scheduler_countdown_enter(void) {}
void scheduler_countdown_leave(void) {}

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
        int has_messages = coop_has_messages(actor);
        int has_timeout = actor->timeout_ns > 0 && actor->last_activity_ns > 0;

        if (has_messages) {
            while (processed < limit) {
                coop_refill(actor);
                if (atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) == 0) break;
                if (!coop_step(actor)) break;   /* stepping higher up the stack, or dead */
                processed++;
            }
        } else if (has_timeout) {
            /* No messages but a countdown is armed: the step checks its
             * deadline by the generated code's clock and fires the arm,
             * disarming it, once it has passed. Only a firing is work: a
             * check counted as one kept wait_for_idle() and the end of main
             * spinning until the deadline (#3219). */
            uint64_t armed_at = actor->last_activity_ns;
            if (coop_step(actor) && actor->last_activity_ns != armed_at) processed++;
        }
        total += processed;
    }

    if (--g_coop_walk_depth == 0) coop_free_released();
    return total;
}

// ============================================================================
// Ask/Reply — simplified synchronous version
// ============================================================================

/* An ask that ends before its message is handled (#3330): the message, still
 * in the target's mailbox or inbox, points at the asker's slot, which lives
 * on the asker's stack and is gone once the ask returns. Its handler would
 * write the reply, or the step's end its failure, into whatever is there
 * then, another ask's slot as like as not. The message stays, as a plain
 * send: it is handled, and its reply goes nowhere, as a late reply does
 * with threads. One step takes one message, so a message the asker no
 * longer waits for is either handled or in one of these two places. */
static void coop_forget_slot(ActorBase* target, ActorReplySlot* slot) {
    Mailbox* mb = &target->mailbox;
    int n = atomic_load_explicit(&mb->count, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        Message* m = &mb->messages[(mb->head + i) & MAILBOX_MASK];
        if (m->_reply_slot == slot) { m->_reply_slot = NULL; return; }
    }
    ActorInbox* ib = atomic_load_explicit(&target->inbox, memory_order_relaxed);
    if (!ib) return;
    actor_inbox_lock(ib);
    for (int i = 0; i < ib->ring_count; i++) {
        Message* m = &ib->ring[(ib->head + i) % AETHER_INBOX_RING];
        if (m->_reply_slot == slot) { m->_reply_slot = NULL; actor_inbox_unlock(ib); return; }
    }
    for (ActorInboxNode* node = ib->first; node; node = node->next) {
        if (node->msg._reply_slot == slot) { node->msg._reply_slot = NULL; break; }
    }
    actor_inbox_unlock(ib);
}

void* scheduler_ask_message_status(ActorBase* target, void* msg_data, size_t msg_size,
                                   int timeout_ms, int* status) {
    int why_dummy;
    if (!status) status = &why_dummy;
    *status = AETHER_ASK_DEAD;
    if (!target || !msg_data) return NULL;
    /* A dead actor answers nothing: no message, no wait (#3151). */
    if (atomic_load_explicit(&target->dead, memory_order_relaxed)) return NULL;

    // Allocate reply slot on stack (single-threaded, no race)
    ActorReplySlot slot = {0};
    slot.reply_data = NULL;
    slot.reply_size = 0;
    slot.reply_ready = 0;
    slot.timed_out = 0;
    atomic_store(&slot.refcount, 1);
    // The asker's timeout (#3330): one thread runs the handler inside the
    // polls below, so a handler slower than the timeout cannot be left; its
    // reply, past the deadline, is late, and the ask has timed out, as with
    // threads, where the asker stops waiting at the deadline.
    slot.deadline_ns = timeout_ms > 0 ? aether_now_ns() + (uint64_t)timeout_ms * 1000000ULL : 0;

    // Its message, with the slot in it (#3395), queued as any send is and
    // processed by the polls below. No memory for the copy fails the ask, and
    // what the message owns is released (#3387).
    Message msg;
    if (!aether_ask_message(target, msg_data, msg_size, &slot, &msg)) {
        aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);   /* a library call ends (#2956) */
        *status = AETHER_ASK_NO_MEMORY;
        return NULL;
    }
    coop_send(target, msg);

    // Process messages until the reply is ready, the deadline passes, or none
    // can come: a poll that handled nothing, with no countdown armed, leaves
    // every mailbox as empty for the next one. The deadline is the only
    // bound in time; a count of polls ended an ask behind a long queue
    // before its timeout (#3330). With no timeout the wait is bounded as
    // scheduler_wait's (100 s). An asker killed meanwhile still gets its
    // answer, or its timeout, here: the slot is on this stack and the
    // message in the target's mailbox points to it, and one thread runs
    // both (#2974).
    uint64_t give_up = slot.deadline_ns ? slot.deadline_ns
                                        : aether_now_ns() + 100ULL * 1000000000ULL;
    while (!slot.reply_ready && !slot.failed && !slot.timed_out) {
        int handled = aether_scheduler_poll(1);
        if (slot.reply_ready || slot.failed || slot.timed_out) break;
        if (aether_now_ns() >= give_up) { slot.timed_out = 1; break; }
        if (handled > 0) continue;
        if (!coop_timeout_armed()) break;
        // Nothing to handle while a countdown runs: wait for it as
        // scheduler_wait does, a millisecond at a time.
        #ifdef _WIN32
        Sleep(1);
        #elif !defined(__EMSCRIPTEN__)
        usleep(1000);
        #endif
    }
    // Ended with its message maybe still queued (the deadline passed behind
    // a slower handler): the message must not keep pointing at this stack.
    if (!slot.reply_ready && !slot.failed) coop_forget_slot(target, &slot);

    if (slot.reply_ready) *status = AETHER_ASK_REPLIED;
    else if (slot.failed) *status = slot.failed;
    else if (atomic_load_explicit(&target->dead, memory_order_relaxed)) *status = AETHER_ASK_DIED;
    else *status = AETHER_ASK_TIMED_OUT;
    return slot.reply_data;
}

void* scheduler_ask_message(ActorBase* target, void* msg_data, size_t msg_size, int timeout_ms) {
    return scheduler_ask_message_status(target, msg_data, msg_size, timeout_ms, NULL);
}

void scheduler_reply_owned(ActorBase* self, void* data, size_t data_size,
                           void (*release)(void*)) {
    (void)self;
    ActorReplySlot* slot = (ActorReplySlot*)g_current_reply_slot;
    g_current_reply_slot = NULL;   /* answered: the step's end has nothing to fail */
    int delivered = 0;
    // A reply past the asker's deadline is late (#3330): the ask timed out.
    if (slot && !slot->timed_out && slot->deadline_ns && aether_now_ns() > slot->deadline_ns)
        slot->timed_out = 1;
    if (slot && !slot->timed_out) {
        slot->reply_data = malloc(data_size);
        if (slot->reply_data) {
            memcpy(slot->reply_data, data, data_size);
            slot->reply_size = data_size;
            delivered = 1;
        }
        slot->reply_ready = 1;
    }
    // Not delivered: what the reply owns stays the replier's to release
    // (#2528); delivered, the bytes are the asker's.
    if (!delivered && release && data) release(data);
}

void scheduler_reply(ActorBase* self, void* data, size_t data_size) {
    scheduler_reply_owned(self, data, data_size, NULL);
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
