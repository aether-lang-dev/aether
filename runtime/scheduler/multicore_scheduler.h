#ifndef MULTICORE_SCHEDULER_H
#define MULTICORE_SCHEDULER_H

#include "../utils/aether_compiler.h"
#include "../utils/aether_thread.h"
#include <stdatomic.h>
#include "../actors/actor_state_machine.h"
#include "../actors/lockfree_mailbox.h"
#include "../actors/aether_adaptive_batch.h"
#include "../actors/aether_message_dedup.h"
#include "../actors/aether_spsc_queue.h"
#include "../config/aether_optimization_config.h"
#include "lockfree_queue.h"
#include "aether_io_poller.h"

/* Sizing. Each is overridable with -D for small-memory targets: a Scheduler
 * holds MAX_CORES + 1 queues of QUEUE_SIZE slots (lockfree_queue.h) and a
 * SCHEDULER_DRAIN_BATCH buffer, about 650 KB at the defaults, and the
 * cooperative scheduler declares MAX_CORES of them while using only [0]. An
 * ESP32 has about 320 KB of DRAM in all. */
#ifndef MAX_ACTORS_PER_CORE
#define MAX_ACTORS_PER_CORE 10000
#endif
#ifndef MAX_CORES
#define MAX_CORES 16
#endif
#define BATCH_SIZE 64  // Process up to 64 messages per batch for better throughput
#ifndef SCHEDULER_DRAIN_BATCH
#define SCHEDULER_DRAIN_BATCH 512  // Drain this many messages at once for high throughput
#endif
#ifndef AETHER_IO_MAX_FDS
#define AETHER_IO_MAX_FDS 4096  // Initial I/O fd map capacity per core (grows on demand)
#endif

// Legacy compatibility - use g_aether_config instead
#define g_sched_features g_aether_config

// Reply slot for ask/reply pattern (experimental)
// Reused across asks (#3533): made once, with its mutex and condition
// variable, and lent to an ask while its count is above zero; at zero both
// the asker and the actor side have let go of it, and the next ask may take
// it. The slots live for the process, on one list, as the reclaim readers do.
typedef struct ActorReplySlot {
    void*              reply_data;   // malloc'd reply payload; returned to caller, caller must free
    size_t             reply_size;   // size of reply_data
    volatile int       reply_ready;  // 1 when reply has been set
    volatile int       timed_out;    // 1 when asker has given up
    volatile int       failed;       // AETHER_ASK_DEAD / _DIED: no reply can come (#3151)
    pthread_mutex_t    mutex;        // protects reply_ready / timed_out / cond
    pthread_cond_t     cond;         // signalled by scheduler_reply()
    atomic_int         refcount;     // 2 while lent (asker + actor); 0: free for the next ask
    uint64_t           deadline_ns;  // the cooperative ask's: a reply after it is late, 0 = none (#3330)
    struct ActorReplySlot* all_next; // every slot made, for the next ask to find a free one
} ActorReplySlot;

// I/O readiness message — delivered to actor when the poller reports fd ready.
// Value must stay inside the 256-slot actor dispatch table (see codegen_actor.c).
// 255 is the top of that range and is reserved for system messages; the Aether
// message registry allocates user message IDs starting at 0 and never hands out 255.
#define MSG_IO_READY 255

/* The death notice (#2974): `message Down { ref: actor_ref, reason: string }`,
 * declared by the compiler for every program that names it, with this
 * reserved id. The runtime writes its payload, so the compiler checks its
 * generated struct against this layout. `reason` is a counted string the
 * message owns. A monitor's Down carries the monitor id in
 * Message.payload_int (0 for a link's). */
#define AETHER_MSG_DOWN 254
typedef struct {
    int _message_id;
    void* ref;
    const char* reason;
} AetherDownMsg;

// TLS pointer to the actor whose step() is currently running on this
// thread. Set by generated *_step() functions at entry; read by the
// reactor bridge (std/net/aether_actor_bridge.c) so await_io() can
// identify the calling actor without a parameter. It is set for every
// step call on every thread.
extern AETHER_TLS void* g_current_step_actor;

/* Accessors for the three thread-local slots the generated program touches.
 *
 * The variables themselves must not be named from generated code. MinGW's GCC
 * chooses per-version whether __thread is a native .tls variable or a
 * __emutls_v. control object, and the two do not link against each other;
 * libaether.a ships prebuilt while the generated C is compiled by whatever GCC
 * the user has, so referencing the variable across that boundary is a coin
 * flip that came up wrong in #1751. A function call has one ABI either way.
 *
 * Neither escape hatch works: GCC on MinGW ignores __declspec(thread)
 * ('thread' attribute directive ignored), which would silently drop the
 * thread-locality rather than fail, and -femulated-tls is a Clang option that
 * GCC rejects outright. */
int  aether_core_id_get(void);
void aether_core_id_set(int core_id);
void aether_reply_slot_set(void* slot);
void aether_step_actor_set(void* actor);

typedef struct {
    int type;       // MSG_IO_READY (must be first field)
    int fd;         // File descriptor that became ready
    uint32_t events; // EPOLLIN, EPOLLOUT, etc.
} IoReadyMessage;

// Per-fd registration: maps fd → actor for I/O dispatch
typedef struct {
    int fd;
    void* actor;        // ActorBase* that receives MSG_IO_READY
    uint32_t events;    // Subscribed events
    int active;         // 1 if slot is in use
} AetherIoEntry;

// Optimized spinlock with PAUSE instruction (cuts wake-up wasted cycles in a busy-spin)
typedef struct {
    atomic_flag lock;
    char padding[63];  // Cache line alignment to prevent false sharing
} OptimizedSpinlock;

static inline void spinlock_init(OptimizedSpinlock* lock) {
    atomic_flag_clear(&lock->lock);
}

static inline void spinlock_lock(OptimizedSpinlock* lock) {
    while (atomic_flag_test_and_set_explicit(&lock->lock, memory_order_acquire)) {
        AETHER_CPU_PAUSE();
    }
}

static inline void spinlock_unlock(OptimizedSpinlock* lock) {
    atomic_flag_clear_explicit(&lock->lock, memory_order_release);
}

struct ActorInbox;   /* runtime/actors/aether_actor_inbox.h */

/* A derived actor is a struct whose first bytes ARE an ActorBase: the
 * scheduler is handed a pointer to it and casts. Anything that mirrors this
 * layout by hand drifts the moment a field is added here, and the corruption
 * is silent, so the fields live in one macro and every mirror expands it. */
/* The fields after the mailbox are in three groups of one cache line each
 * (#3540), on x86_64 and every other target whose pointers are 8 bytes: the
 * actor's block is 64-byte aligned (AETHER_ACTOR_ALIGN) and the mailbox
 * ends 40 bytes before a line, so
 *  - the end of that line holds what the core stepping the actor writes or
 *    reads at every step and no other thread touches on a send (the step,
 *    the step lock, the payload in hand), beside the mailbox's head, tail
 *    and count, which only that core writes;
 *  - the next line holds what a sender on another core reads or writes on
 *    every send (dead, the mailbox bound, assigned_core, auto_process,
 *    inflight, migrate_to, the bound's count), which the stepping core
 *    pulls back once per delivery anyway, as it uncounts inflight there;
 *  - the last holds what changes at spawn, death or a receive timeout.
 * Senders' reads of assigned_core and dead used to share the lines the
 * stepping core writes at every step (the mailbox count's, step_lock's), so
 * each send from another core cost the next step transfers of those lines
 * back to its core. sizeof(ActorBase) is 1712, as before, so an actor's
 * state starts where it did. On a target with 4-byte pointers the groups
 * fall elsewhere on their lines; nothing reads a field by its offset. */
#define AETHER_ACTOR_BASE_FIELDS \
    atomic_int active; \
    int id; \
    Mailbox mailbox; \
    /* ---- The stepping core's, beside the mailbox's head, tail, count ---- */ \
    void (*step)(void*); \
    /* The payload of the message a handler is running on (its heap copy, \
     * or the actor's copy of one that travelled in the message, as \
     * step_inline says), and its type: the generated step sets them around \
     * the handler call and clears the payload after it. A panic out of the \
     * handler skips the release that follows, so the step barrier drops \
     * the message (#3142). NULL between handlers. */ \
    void* step_payload; \
    /* Non-NULL only while an ask/reply is in flight */ \
    _Atomic(ActorReplySlot*) reply_slot; \
    /* Where the actor sits in its core's timer heap (#3540), index + 1, \
     * 0 while it is in none: its receive timeout is not armed, or the \
     * core has not been told yet. Written by its core's thread only; the \
     * reclaim reads it, and keeps a released block while it is not 0. */ \
    atomic_int timer_slot; \
    int step_type; \
    /* Prevents concurrent step() calls during work-steal handoff. \
     * Set (TAS) by the thread about to call step(); cleared (release) after. \
     * Work-stealing threads that find this set retry next outer iteration. */ \
    atomic_flag step_lock; \
    /* #3534: 1 while step_payload is the actor's own copy of a payload \
     * that travelled in its message (a generated actor's \
     * _aether_step_inline, at the end of its struct), which the step \
     * barrier reads but never frees; 0 while it is a heap copy. Written \
     * and read only by the thread stepping the actor. */ \
    unsigned char step_inline; \
    /* AETHER_EXIT_* bits (#2974). */ \
    _Atomic(unsigned char) exit_flags; \
    /* 1 while one entry for the actor waits in a core's ready list \
     * (#3540), the list its core's loop looks at instead of every actor \
     * it has: set by whoever pushes the entry, cleared by the core that \
     * takes it, both with an exchange. Its own core pushes and takes it; \
     * another thread pushes it seldom (a move, a steal, a restart). */ \
    _Atomic(unsigned char) ready_queued; \
    /* 1 when scheduler_spawn_actor allocated the actor, which the scheduler \
     * then frees; 0 for one a caller passed to scheduler_register_actor, \
     * whose memory stays the caller's. Written before the actor is \
     * published, never after. */ \
    int scheduler_owned; \
    /* ---- What a send from another core reads or writes ---- */ \
    /* Bits (#2517): AETHER_ACTOR_PANICKED once the actor's step() unwound \
     * via aether_panic() or a caught signal, AETHER_ACTOR_RELEASED once it \
     * was released, AETHER_ACTOR_THREAD_GONE once its own thread (an \
     * auto_process actor's) has left its loop, AETHER_ACTOR_KILLED once a \
     * kill or a linked actor's death ended it, AETHER_ACTOR_STOPPED once \
     * it stopped itself (#2974). Non-zero actors are skipped by the \
     * scheduler and incoming messages are dropped. Bits are only ever \
     * set, but by a restart in place (#2974), which clears the death \
     * bits (never RELEASED) once the new incarnation is ready. */ \
    atomic_int dead; \
    /* The mailbox bound (#2974, `@mailbox(bound: N)`): at most this many \
     * messages accepted for the actor and not yet taken by its step, 0 for \
     * none. Set by the generated spawn; read by every send. */ \
    atomic_int mailbox_bound; \
    atomic_int assigned_core; \
    int auto_process; \
    /* Messages for this actor on their way through a core's queues or a \
     * sender's overflow buffer: counted before the sender reads \
     * assigned_core, uncounted when the message reaches the mailbox or \
     * the actor's overflow (#3090, #3093, #3241). The actor moves to \
     * another core only while this is \
     * 0, with AETHER_INFLIGHT_MOVING set for the move, so no message is \
     * left behind on the core it leaves. */ \
    atomic_int inflight; \
    /* Affinity hint: core to migrate to (-1 = none) */ \
    atomic_int migrate_to; \
    /* With a mailbox bound (#2974): the messages accepted for it and not \
     * yet taken by its step or dropped, wherever they are (a core's queue, \
     * a sender's deferred sends, its overflow, its mailbox). System \
     * messages (Down, IoReady) are neither counted nor refused. */ \
    atomic_int mailbox_pending; \
    /* Plain sends dropped because its mailbox was full (#2974). */ \
    atomic_uint full_drops; \
    /* 1 while the actor's own thread sleeps in its idle park (#2592): a \
     * sender that has just put a message in the inbox and reads 1 wakes \
     * the thread. */ \
    atomic_int thread_parked; \
    /* How many times the actor was restarted in place, under the same ref \
     * (#2974): 0 for the actor spawned, one more at each rebirth. */ \
    atomic_int incarnation; \
    /* An auto_process actor's inbox (#2598): every sender puts its \
     * messages here and only the actor's thread takes them. Made by the \
     * first send, NULL until then. For an actor a core runs, its overflow \
     * (#3241): a waiting core's relief starts it with the messages a full \
     * mailbox could not take, every delivery goes behind it while it holds \
     * anything, and each step refills the mailbox from it. Made by the \
     * first such relief, NULL until then. */ \
    _Atomic(struct ActorInbox*) inbox; \
    /* Its death notices (#2974): the actors that monitor it or are linked \
     * to it, the ones it monitors, and why it ended \
     * (runtime/actors/aether_supervision.c, aether_actor_death_reason). \
     * Made by the first monitor, link or death, freed with the actor. */ \
    _Atomic(struct AetherWatch*) death_watch; \
    pthread_t thread; \
    /* ---- Set at spawn, death or a receive timeout ---- */ \
    /* Receive timeout (#3162): the interval a `receive ... after` names, \
     * in ns, set at spawn and kept; 0 = the actor has none. Atomic, as \
     * last_activity_ns is: scheduler_wait reads both on its own thread. */ \
    _Atomic uint64_t timeout_ns; \
    /* When its countdown was armed, 0 while it is not: the spawn and the \
     * end of each message's handling arm it, a message and the firing \
     * disarm it (the generated step function does all three). Written on \
     * the thread that steps the actor, read by scheduler_wait's scan for an \
     * armed countdown (has_pending_actor_timeout) on the waiting thread: \
     * atomic, both sides, or the scan raced the step. */ \
    _Atomic uint64_t last_activity_ns; \
    /* Full allocation size passed to scheduler_spawn_actor. numa_free() unmaps \
     * exactly [ptr, ptr+size), so freeing a derived actor with \
     * sizeof(ActorBase) would leak the derived-struct tail under libnuma. */ \
    size_t alloc_size; \
    /* Releases what the actor's state fields own (heap strings, closure \
     * environments, owning structs), set by the generated spawn; NULL for \
     * an actor registered by a caller. Called exactly once, by the free \
     * path a release or the scheduler's teardown ends the actor through, \
     * after the actor can no longer be stepped (#2528). */ \
    void (*destroy_state)(void*); \
    /* Releases what a message's fields own (strings, closures, structs) \
     * when it is dropped instead of handled, by the program's message \
     * definitions: the receiver's, which read its type as its dispatch \
     * does. Set by the generated spawn; NULL for an actor registered by a \
     * caller, or in a program whose messages own nothing. Called through \
     * aether_message_drop() only (#3142). */ \
    void (*release_message)(int type, void* payload); \
    /* NULL: the send buffer's same-core batches went here, outside every \
     * ordering rule, and were drained only while the mailbox held \
     * something; it sends through scheduler_send_many now (#3292). Kept \
     * for the layout; an actor's end frees one a C caller set. */ \
    SPSCQueue* spsc_queue;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
} ActorBase;

/* #3534: the payload the handler of `msg` runs on, in a generated actor's
 * step: one that travelled in the message copied to `copy` (the actor's
 * _aether_step_inline, where its fields are aligned and a panic that
 * unwinds the step still finds it), a heap one where payload_ptr points,
 * NULL for none. step_inline says which, for the step barrier. One copy
 * per actor is enough: an actor is never stepped inside its own step
 * (#3391). */
static inline void* aether_message_take_payload(ActorBase* actor, Message* msg,
                                                AetherMsgInline* copy) {
    if (aether_message_is_inline(msg)) {
        memcpy(copy->bytes, aether_message_inline_bytes(msg), AETHER_MSG_INLINE_BYTES);
        actor->step_inline = 1;
        return copy->bytes;
    }
    actor->step_inline = 0;
    return msg->payload_ptr;
}

/* #3534: once the handler is done and the message's fields released, a heap
 * payload is freed; the actor's copy of an inline one is not. */
static inline void aether_message_payload_done(void* payload, AetherMsgInline* copy) {
    if (payload && payload != (void*)copy->bytes) aether_free_message(payload);
}

struct AetherWatch;  /* runtime/actors/aether_supervision.c */

/* The bits of ActorBase.dead; a send checks only that it is non-zero. */
#define AETHER_ACTOR_PANICKED 1
#define AETHER_ACTOR_RELEASED 2
#define AETHER_ACTOR_THREAD_GONE 4
#define AETHER_ACTOR_KILLED 8
#define AETHER_ACTOR_STOPPED 16
/* The bits that end an actor: the first one set decides its reason. */
#define AETHER_ACTOR_ENDED (AETHER_ACTOR_PANICKED | AETHER_ACTOR_RELEASED | \
                            AETHER_ACTOR_KILLED | AETHER_ACTOR_STOPPED)

/* ActorBase.exit_flags (#2974): linked deaths reach it as Down messages
 * (trap_exits), and its death has been finalized (drained, its watchers
 * told), which happens once. */
#define AETHER_EXIT_TRAP      1
#define AETHER_EXIT_FINALIZED 2
/* A caller's actor that a core may be delivering to while it is in no table
 * (#3673): one carried over a scheduler restart (teardown sets it, and its
 * assigned_core still names the core its messages go to), or one a core
 * took because its assigned_core named no core. scheduler_register_actor
 * keeps such an actor on that core, which is the one writer of its mailbox,
 * and clears the bit. */
#define AETHER_EXIT_ROUTED    4

/* ActorBase.inflight (#3090, #3093): set while the actor's core moves it to
 * another; the counts of messages in flight sit below it. */
#define AETHER_INFLIGHT_MOVING (1 << 30)

/* A sender about to route `n` messages for `actor` through a core's queues
 * counts them first, then reads assigned_core: while the count is not 0 the
 * actor stays on its core, so the messages reach it there. During a move it
 * waits for the move to end, and then reads the new core. */
static inline void inflight_enter(ActorBase* actor, int n) {
    int v = atomic_fetch_add_explicit(&actor->inflight, n, memory_order_seq_cst);
    while (unlikely(v & AETHER_INFLIGHT_MOVING)) {
        AETHER_CPU_PAUSE();
        v = atomic_load_explicit(&actor->inflight, memory_order_acquire);
    }
}

/* `n` counted messages reached the actor's mailbox or overflow (or were
 * dropped). */
static inline void inflight_leave(ActorBase* actor, int n) {
    atomic_fetch_sub_explicit(&actor->inflight, n, memory_order_release);
}

/* Generated actor structs are declared aligned(64) (codegen_actor.c), one
 * cache line, so the runtime allocates every actor on this boundary (#2485). */
#define AETHER_ACTOR_ALIGN 64

/* A core's actor table (#2486). Scheduler threads and the main thread read it
 * without the core's actor_lock, so the slots are atomic and a table that is
 * grown out of is kept, chained on `retired`, until scheduler_cleanup(). The
 * protocol is described above the helpers in multicore_scheduler.c. */
typedef struct AetherActorTable {
    struct AetherActorTable* retired;  // the smaller table this one replaced
    size_t alloc_size;                 // bytes, for aether_numa_free
    int capacity;
    _Atomic(ActorBase*) slots[];
} AetherActorTable;

typedef struct {
    int core_id;
    pthread_t thread;
    _Atomic(AetherActorTable*) actor_table;
    _Atomic int actor_count;
    // The reclamation epoch this core's thread last observed at the top of
    // its loop, or 0 while the thread is not running (#2509). A released
    // actor is freed only once every core is past the epoch it was released
    // in; see the reclamation notes in multicore_scheduler.c.
    _Atomic uint64_t reclaim_epoch;
    // Per-sender SPSC channels: from_queues[src] is written ONLY by core src.
    // Each channel is a true SPSC queue, so no CAS or locks are needed on the
    // producer side. from_queues[MAX_CORES] is the channel for every thread
    // that is NOT a scheduler core — the main thread, a std.http pool worker,
    // a std.worker thread, a C library's callback thread — and those are many
    // producers, serialized by `foreign_lock` so the channel still has one
    // producer at a time (#2083: a pool-thread `!` raced the main thread on
    // this slot and the message was silently lost).
    LockFreeQueue from_queues[MAX_CORES + 1];
    pthread_mutex_t foreign_lock;
    atomic_int running;
    // The messages this core has taken from its queues, a measure of its
    // load for placement and work stealing. Counted by the core itself as it
    // drains (#3541), its one writer: a sender added to it on every send,
    // an atomic add on a line of the receiving core's.
    atomic_int work_count;
    atomic_int steal_attempts;  // Cumulative count of successful work-steal operations
    atomic_int idle_cycles;     // Track how long core has been idle
    OptimizedSpinlock actor_lock;  // Serializes every writer of actor_table / actor_count

    // Per-core message counters, read cross-thread by
    // count_pending_messages(), so _Atomic to avoid torn reads on
    // weakly-ordered architectures (e.g. ARM64). Both are written by this
    // core's thread alone, with a load and a release store (#3541); other
    // threads count on shared atomic counters (count_sent, count_processed).
    _Atomic uint64_t messages_sent;      // Messages sent FROM this core
    _Atomic uint64_t messages_processed; // Messages processed ON this core
    char counter_padding[48];    // Cache line padding to prevent false sharing

    // Message coalescing buffer — amortises enqueue atomics across bursts
    struct {
        void* actors[SCHEDULER_DRAIN_BATCH];
        Message messages[SCHEDULER_DRAIN_BATCH];
        int count;
        /* While a step runs from the batch, the first message not yet in a
         * mailbox: [next, count) is older than anything still queued, and a
         * relief delivers nothing for an actor with a message there
         * (#3197). `count` once the batch is through. */
        int next;
    } coalesce_buffer;

    AdaptiveBatchState batch_state;   // Adaptive batching (small, embedded)

    // Idle park (#1517). A core with no work and nothing registered with the
    // poller sleeps here instead of spinning. The sleeper sets `parked` and
    // then re-checks its queues, so a producer that reads parked==0 has
    // already made its message visible to that check. The wait is timed as
    // well, so a path that never signals costs latency, never a hang.
    pthread_mutex_t park_mutex;
    pthread_cond_t  park_cond;
    atomic_int      parked;
    // 1 once a message went into one of this core's queues since the core
    // last drained them (#3541): its loops that step an actor many times
    // read this one word to yield to the queues, not the head and tail of
    // all of them. Set by sched_wake, on the line the producer reads
    // `parked` from, and by this core's own requeues; cleared by the core
    // as it drains. A hint: the park still reads every queue.
    atomic_int      inbound;

    // How many asks this core's thread is blocked in (#3220): while one is,
    // the core's loop does not run, so no actor is moved onto it
    // (move_actor), but the one it waits on, by a relief (#3197).
    atomic_int      asking;
    // The innermost of those asks: the actor it waits on and its reply
    // slot, set and put back under actor_lock with `asking`. Another core's
    // relief moves that actor here, where the waiting thread steps it, and
    // wakes the wait through the slot (#3197). And the actor that asked,
    // whose kill wakes the wait through the slot too (#2974).
    _Atomic(ActorBase*)      ask_target;
    _Atomic(ActorReplySlot*) ask_slot;
    _Atomic(ActorBase*)      ask_asker;
    // Actors of this core whose death is to be finalized here (#2974): a
    // kill or a link's from another thread posts them, and the core's loop
    // (or its relief) finalizes each once it is not stepping.
    atomic_flag     control_lock;
    atomic_int      control_pending;
    ActorBase**     control;
    int             control_count;
    int             control_cap;
    // 1 once the core's thread has left its loop: it takes no more finalize
    // requests, and a death posted then is finalized by its poster. Under
    // control_lock; 0 again at scheduler_init.
    int             control_closed;
    // Where this core's next relief scan of its table starts (#3197).
    int             relief_cursor;

    // Per-core I/O event loop (platform-agnostic: epoll/kqueue/poll)
    AetherIoPoller io_poller;         // Platform I/O poller instance
    AetherIoEntry* io_map;            // fd → actor mapping (heap-allocated, grows on demand)
    int io_map_capacity;              // Current allocated size of io_map
    int io_registered_count;          // Number of active I/O registrations

    // The ready list and the timer heap (#3540): what the core's loop looks
    // at instead of every actor it has. See the notes above scheduler_thread
    // in multicore_scheduler.c. All of it but `ready_in` is this core's
    // thread's alone.
    ActorBase** ready;                // the actors to look at in the next pass
    int ready_count;
    int ready_cap;
    ActorBase** ready_work;           // the pass under way: [ready_at, ready_work_count)
    int ready_work_count;
    int ready_work_cap;
    int ready_at;
    struct AetherTimer* timers;       // a min-heap of receive timeout deadlines
    int timer_count;
    int timer_cap;
    // Entries pushed by other threads, under `lock`; taken into `ready` by
    // the core. On a line of their own: other threads write it.
    struct {
        atomic_flag lock;
        atomic_int count;
        ActorBase** list;
        int cap;
    } AETHER_ALIGNED(64) ready_in;
} Scheduler;

extern Scheduler schedulers[MAX_CORES];
extern int num_cores;
extern atomic_int next_actor_id;

// Initialize scheduler with core count (autodetects hardware)
void scheduler_init(int cores);

// Initialize with explicit optimization flags
void scheduler_init_with_opts(int cores, AetherOptFlags opts);

void scheduler_start(void);

// The CPU scheduler thread `core_id` is placed on: the (core_id mod n)-th
// of the n CPUs the process may run on, read when the threads last started
// (#3106). -1 when that set could not be read or the platform has no CPU
// numbers to place by (macOS).
int scheduler_core_cpu(int core_id);

void scheduler_stop(void);
void scheduler_wait(void);      // Wait for quiescence (all pending messages processed). Non-destructive.
/* The panic scheduler_wait() raises inside an actor's step, on either
 * scheduler, and on the threaded one on any thread that runs actors (a
 * core's, an actor's own), where a drop or a death hook runs too (#3463): it
 * waits for every actor, those this thread runs too. */
#define AETHER_WAIT_IN_STEP_REASON                                                  \
    "wait_for_idle() inside an actor, or on a thread that runs actors: it waits "   \
    "for every actor, those this thread runs too, which cannot step while it "      \
    "waits; call it outside the actors (main, a host's thread)"
void scheduler_shutdown(void);  // Wait + stop + join threads. Call once at program exit.
void scheduler_cleanup(void);

/* An actor the caller allocated: zeroed before its first registration
 * (calloc, or memset), with its mailbox initialised and its step set. It
 * may be registered again after a shutdown; its count of messages in
 * flight carries over (#3090). */
int scheduler_register_actor(ActorBase* actor, int preferred_core);
/* Out of its core's table. An entry for it may still wait in the core's
 * ready list or timer heap (#3540), which the core takes and drops once it
 * sees the actor released: scheduler_release_actor, which ends an actor
 * through this, keeps the block until then. A caller that deregisters an
 * actor with work left and frees it at once leaves the core that entry. */
void scheduler_deregister_actor(ActorBase* actor);
void scheduler_send_local(ActorBase* actor, Message msg);
void scheduler_send_remote(ActorBase* actor, Message msg, int from_core);
/* `n` messages from this thread to one actor, in order, as `n` sends one
 * after another would go: counted as sent, behind whatever is queued for the
 * actor, and kept when a queue is full. To an actor of another core, from a
 * core's thread, they go in one batch (#3292). From any thread. */
void scheduler_send_many(ActorBase* actor, const Message* msgs, int n);

// Batch send for main thread fan-out patterns (fork-join)
void scheduler_send_batch_start(void);
void scheduler_send_batch_add(ActorBase* actor, Message msg);
void scheduler_send_batch_flush(void);

// NUMA-aware actor lifetime (TIER 1 - always on)
ActorBase* scheduler_spawn_actor(int preferred_core, void (*step)(void*), size_t actor_size);
/* Ends an actor (#2509): marks it released, takes it out of its core's table
 * and reclaims it once no scheduler thread or table reader can still be
 * looking at it, which is later, not during the call. Safe to call from the
 * actor's own step, and it never blocks: an actor with its own thread
 * (auto_process) is only marked here, and that thread, which leaves its
 * loop at the mark, ends the actor itself once it has let go of it.
 * Nothing may send to the actor after this. A
 * message that still arrives is dropped: the actor's memory stays allocated,
 * marked released, until a later spawn of the same size reuses it (#2517),
 * so a late send is defined, and one that comes after the reuse reaches the
 * new actor. */
void scheduler_release_actor(ActorBase* actor);
/* Ends every released actor that can be ended now (its state destroyed, its
 * block kept for reuse), waiting a bounded time for the core threads to move
 * past the retired ones. Returns the number still retired: 0 means every
 * release so far has been ended; non-zero means a reader (a core blocked in
 * a long step, a walk on this thread) still holds some back, and a caller
 * that needs them ended calls again. The cooperative scheduler ends them on
 * the spot unless a walk or an inline send is on the stack (#2528). */
int scheduler_reclaim_released(void);
/* Released actors not reclaimed yet, for tests and diagnostics (#2509). */
int scheduler_released_actors_pending(void);
/* Messages dropped because their target had been released (#2517): each is
 * a send made against the release contract. */
uint64_t scheduler_released_sends(void);

// Sends from a thread the scheduler does not own while it was not running
// (stopped, starting again, cleaned up): dropped, their payload released,
// and reported once on stderr (#2912).
uint64_t scheduler_stopped_sends(void);

/* ------------------------------------------------------------------------
 * Death notices (#2974): runtime/actors/aether_supervision.c and the
 * schedulers' finalize. An actor ends once, by its first death bit: a
 * panic, a kill (or a linked actor's death), a stop, a release. Its death
 * is finalized on its own thread once it is not stepping (drained, its
 * watchers sent Down), so no Down comes for an actor whose handler still
 * runs.
 * ------------------------------------------------------------------------ */

/* Messages a panicked, killed or stopped actor did not handle: queued at its
 * death or sent to it after. Each was released through aether_message_drop. */
uint64_t scheduler_dead_sends(void);
void scheduler_count_dead_sends(int n);

/* ------------------------------------------------------------------------
 * A bounded mailbox (#2974): `@mailbox(bound: N)` on an actor. A send
 * (scheduler_send_local, scheduler_send_remote and the paths built on
 * them) is accepted while fewer than N messages are pending for the actor
 * and refused otherwise, never blocked: a plain send is dropped, released
 * and counted (full_drops, reported once on stderr), an ask fails at once
 * with AETHER_ASK_FULL, and scheduler_try_send says so to its caller. The
 * generated step of a bounded actor gives a message back as it takes it
 * (aether_mailbox_took), and every drop of one not taken gives it back
 * (aether_mailbox_untake), so `mailbox_pending` is exact. System messages
 * are never refused nor counted.
 * ------------------------------------------------------------------------ */

static inline int aether_message_is_system(int type) {
    return type == AETHER_MSG_DOWN || type == MSG_IO_READY;
}

/* Takes a place for one message of `type`: 1 when accepted. Exact: a send
 * is refused only while `bound` messages are pending. */
static inline int aether_mailbox_admit(ActorBase* a, int type) {
    int bound = atomic_load_explicit(&a->mailbox_bound, memory_order_relaxed);
    if (bound == 0 || aether_message_is_system(type)) return 1;
    int p = atomic_load_explicit(&a->mailbox_pending, memory_order_relaxed);
    while (p < bound) {
        if (atomic_compare_exchange_weak_explicit(&a->mailbox_pending, &p, p + 1,
                                                  memory_order_relaxed, memory_order_relaxed))
            return 1;
    }
    return 0;
}

/* A message of `type` that was accepted is dropped before its step took it. */
static inline void aether_mailbox_untake(ActorBase* a, int type) {
    if (a && atomic_load_explicit(&a->mailbox_bound, memory_order_relaxed) != 0 &&
        !aether_message_is_system(type))
        atomic_fetch_sub_explicit(&a->mailbox_pending, 1, memory_order_relaxed);
}

/* The generated step of a bounded actor, after it takes a message. */
static inline void aether_mailbox_took(ActorBase* a, int type) {
    if (!aether_message_is_system(type))
        atomic_fetch_sub_explicit(&a->mailbox_pending, 1, memory_order_relaxed);
}

/* Sends `msg` unless its target is dead or its mailbox is full, and says
 * which: "" when sent, "the mailbox is full" or "the actor is dead" when
 * not, the message released (and counted nowhere: the caller knows).
 * `try_send(ref, Msg {...})` (aether_try_send_message for a payload). */
const char* scheduler_try_send(ActorBase* actor, Message msg);
const char* aether_try_send_message(void* actor, void* message_data, size_t message_size);

/* Plain sends dropped because their target's mailbox was full. */
uint64_t scheduler_full_drops(void);

/* Ends `actor` with `reason` by death bit `bit` (KILLED, STOPPED), unless it
 * ended already; returns 1 when this call ended it. Its finalize runs on its
 * own thread once its current step (if any) returns. From any thread. */
int scheduler_actor_end(ActorBase* actor, const char* reason, int bit);

/* Claims `actor`'s death for `reason` and sets `bit`: 1 when nothing ended
 * it before, else 0 and nothing changes but the bit. */
int aether_actor_claim_end(ActorBase* actor, const char* reason, int bit);
/* Keeps `reason` as the actor's death reason unless one is kept already. */
void aether_actor_claim_reason(ActorBase* actor, const char* reason);
/* Why the actor ended, kept with its watch list: "normal", "killed", the
 * panic's reason or a linked actor's; NULL while it lives. Set by whatever
 * ended it first, before its death bit. */
const char* aether_actor_death_reason(ActorBase* actor);
/* An ended actor's death, finalized on the thread that owns it (an actor
 * thread's own, at its loop's end). */
void scheduler_actor_thread_finalize(ActorBase* actor);
/* Tells the actor's watchers of its death (once), and lets go of the
 * actors it watches. Any thread. */
void aether_watch_notify(ActorBase* actor, const char* reason);
/* Frees the actor's watch list and death reason (its block is ending). */
void aether_watch_free(ActorBase* actor);
/* A Down for `self` from a monitor `self` has since dropped (demonitor) is
 * not handled: 0 for it, 1 for every other. Called by the generated step
 * before a Down arm. */
int aether_down_accept(ActorBase* self, Message* msg);

/* The std.actors surface, from a handler (the actor stepping on this
 * thread is `self`). On wasi a panic ends the program (it has no setjmp),
 * so a death a monitor would be told of cannot be caught there: a call to
 * these is refused at compile time on that target. */
#if defined(__wasi__) && defined(__clang__)
#define AETHER_NEEDS_PANIC_BARRIER \
    __attribute__((unavailable("watching a death needs the step's panic barrier, and on wasi " \
                               "a panic ends the program (no setjmp)")))
#else
#define AETHER_NEEDS_PANIC_BARRIER
#endif
int     aether_actor_monitor(void* ref) AETHER_NEEDS_PANIC_BARRIER;
void    aether_actor_demonitor(int id);
void    aether_actor_link(void* ref) AETHER_NEEDS_PANIC_BARRIER;
void    aether_actor_unlink(void* ref);
void    aether_actor_trap_exits(int on) AETHER_NEEDS_PANIC_BARRIER;
void    aether_actor_stop(void);
void    aether_actor_kill(void* ref);
int64_t aether_actor_dead_sends(void);
int64_t aether_actor_full_drops(void* ref);

/* Restart in place (#2974): a supervisor reruns a child's factory, and the
 * factory's spawn of the child's type takes the child's own block, so the
 * ref its clients hold reaches the new incarnation. rebirth_begin claims an
 * ended, finalized child with no message of it in flight (1; 0: not yet, try
 * again; -1: alive, released, or not the scheduler's); the spawn then resets
 * the block (its old state destroyed, its fields zeroed, its watch lists
 * gone, incarnation + 1) and the generated spawn's state initializers run;
 * scheduler_spawn_ready, the generated spawn's last line, brings it alive.
 * rebirth_end says whether it is (1), or undoes a rebirth the factory did
 * not finish (0: it spawned no actor of the type, or panicked), the child
 * left ended. A send while it is dead is dropped and counted; one after the
 * spawn returns reaches the new incarnation. */
int  aether_actor_rebirth_begin(void* ref);
int  aether_actor_rebirth_end(void* ref);
int  aether_actor_incarnation(void* ref);
/* The block scheduler_spawn_actor reuses for a rebirth on this thread, or
 * NULL. */
ActorBase* aether_rebirth_take(void (*step)(void*), size_t size);
void scheduler_spawn_ready(ActorBase* actor);
/* A panic left a rebirth half done on this thread: undone. */
void aether_rebirth_abandon(void);
/* The step barrier's part (#3389, aether_step_barrier.h): a step nested in
 * another puts the thread's restart claim, reborn block and spawn brackets
 * aside (1 when it held any), and gets them back when it ends; a step that
 * panicked undoes the claim and closes the brackets it made itself. */
typedef struct {
    ActorBase* target;
    ActorBase* reborn;
    int capture_floor;
} AetherSpawnAside;
int  aether_spawn_put_aside(AetherSpawnAside* s);
void aether_spawn_give_back(const AetherSpawnAside* s);
void aether_spawn_step_panicked(void);
/* The scheduler's part once a reborn actor is alive: its core is told, and
 * looks at it (its receive timeout, its mailbox). */
void scheduler_actor_revived(ActorBase* actor);
/* A receive timeout armed outside the actor's own step: its core is told,
 * which fires it once it is due (#3540). A core looks only at the actors it
 * was told of, not at every actor it has, so a countdown armed by its step
 * needs nothing (the core reads it after the step), and the generated
 * spawn's arming is told by scheduler_spawn_ready. C code that arms an
 * actor's timeout_ns and last_activity_ns by hand calls this after. */
void scheduler_timeout_armed(ActorBase* actor);
/* Live restart claims and spawn captures, on any thread, are counted in
 * aether_caps_slow (aether_spawn_watched, aether_resource_caps.h). */
/* The child a supervisor's factory spawned (#2974): the first actor that
 * comes alive on this thread between begin and end (a fresh spawn or a
 * rebirth), or NULL. Brackets nest. */
void  aether_actor_capture_begin(void);
void* aether_actor_capture_end(void);
void  aether_actor_capture_note(ActorBase* actor);

// #2527: a reference that knows which actor it was taken from. The block a
// released actor leaves goes to the next spawn of its size, which gets an id
// of its own; a send through a reference taken from the released actor is
// then dropped and counted by scheduler_released_sends(), rather than
// delivered to the new actor, as a send through the bare pointer is.
typedef struct {
    ActorBase* actor;
    int id;
} AetherActorRef;

AetherActorRef scheduler_actor_ref(ActorBase* actor);
void scheduler_send_ref(AetherActorRef ref, Message msg);

// A message dropped instead of handled (#3142): what its fields own goes
// through `target`'s release_message, then its payload and an owned zero-copy
// buffer are freed. Every drop goes through here: a dead or released target's
// mailbox and queues, a send refused, a type the receiver has no handler for.
// The reply slot and the processed count are the caller's. `target` may be
// NULL (a reference whose block is another actor's now): the payload alone
// is freed.
void aether_message_drop(ActorBase* target, Message* msg);
/* #3401: aether_message_drop of a message an ending actor leaves
 * undelivered, as its finalize drains it on either scheduler: the release
 * runs in a frame of its own, so a panic in it (a field's drop function is
 * the program's code) is reported through the death hook, as the step
 * barrier reports one in a release after a handler, and the end goes on. */
void aether_message_drop_at_end(ActorBase* target, Message* msg);
/* The drop alone, in its containment frame (#3403): 1 when the release
 * panicked (what it left is freed; *reason and *reason_release are the
 * panic's, the caller's to report and release), else 0. The step barrier
 * drops the message a panicking handler ran on through it. */
int aether_message_drop_contained(ActorBase* target, Message* msg, const char** reason,
                                  void (**reason_release)(const void*));
/* A thread that steps actors without being a core's scheduler thread
 * (aether_actor_thread, which inlines sends to actors of its core) takes
 * part in reclamation like a core (#2517): it registers before its first
 * step, publishes the epoch it reads in at every quiescent point (the top of
 * its loop, holding no actor it found through a table), and deregisters
 * when it is done. A released actor is not reclaimed while such a thread
 * may still hold it. `actor` is the one the thread serves, or NULL. */
void scheduler_reader_online(void);
void scheduler_reader_quiescent(void);
void scheduler_reader_offline(void);
/* The last thing an auto_process actor's thread does (#2517): the actor is
 * ended, as a release ends any other, if it has been released; a release
 * that comes later ends it then. */
void scheduler_actor_thread_exit(ActorBase* actor);
/* An auto_process actor's thread with nothing to do sleeps here (#2592),
 * after spinning a while, instead of spinning for good: until a message for
 * the actor, its release, the scheduler's stop or its teardown wakes it,
 * with a timed wait behind those. While asleep the thread is not a reader,
 * so it holds no reclamation back. Returns 0 without sleeping when there is
 * nothing to sleep on: the actor is on no core, or no scheduler has been
 * initialized yet. 1 otherwise. */
int scheduler_actor_thread_park(ActorBase* actor);
/* One step of `actor` as a core takes it: inside the panic barrier, with a
 * step context of its own (the ask it handles, answered or failed when the
 * step ends), and the caller's given back after. What an auto_process
 * actor's thread steps it with. The caller holds what a core holds to step
 * it. 1 when the step took a message, which the caller counts handled (#3464). */
int scheduler_step_actor(ActorBase* actor);
/* Around a step that fires a receive timeout (#3443): counted until its arm
 * returns, so wait_for_idle() waits for the arm, which runs after the step
 * disarmed the countdown. */
void scheduler_countdown_enter(void);
void scheduler_countdown_leave(void);
/* `n` messages handled (or dropped) on this thread, for scheduler_wait()'s
 * count (#3541): a core's thread counts on its own core, any other thread on
 * a count those threads share. An actor thread credits each step that took
 * a message here. */
void scheduler_count_processed(int n);
/* Puts `msg` in an auto_process actor's inbox and wakes its thread if it
 * sleeps (#2598, #2592). From any thread; the message is counted as sent by
 * the caller. Every send to such an actor ends here. */
void scheduler_actor_thread_deliver(ActorBase* actor, Message msg);
/* The timed wait behind an actor thread's park, in milliseconds (32 by
 * default). For tests: set long, a wake that never comes shows as a stall
 * rather than as a 32 ms delay. */
void scheduler_set_actor_park_ms(int ms);
/* The timed wait behind a core's park, in milliseconds; 0, the default,
 * steps it from 1 to 32 ms the longer the core is idle. For tests, as the
 * one above: set long, a core that is never woken stalls (#3087). */
void scheduler_set_core_park_ms(int ms);

// Ask/reply: send a message and block until a reply arrives or timeout.
// Returns malloc'd reply payload on success (caller must free), NULL when
// there is none.
void* scheduler_ask_message(ActorBase* target, void* msg_data, size_t msg_size, int timeout_ms);

/* Why an ask got no reply (#3151), the *status scheduler_ask_message_status
 * gives: `v, err = actor ? Msg {}` turns it into err. An ask to a dead actor
 * ends at once; one whose actor dies with it in hand ends when it dies. */
#define AETHER_ASK_REPLIED    0
#define AETHER_ASK_TIMED_OUT  1   /* no reply within timeout_ms */
#define AETHER_ASK_DEAD       2   /* the actor was dead, or died before the ask reached it */
#define AETHER_ASK_DIED       3   /* the actor died handling the ask, before it replied */
#define AETHER_ASK_NO_MEMORY  4   /* the ask could not be made */
#define AETHER_ASK_NO_REPLY   5   /* the handler returned without replying */
#define AETHER_ASK_KILLED     6   /* the asking actor was killed while it waited (#2974) */
#define AETHER_ASK_FULL       7   /* the actor's mailbox bound was reached (#2974) */
void* scheduler_ask_message_status(ActorBase* target, void* msg_data, size_t msg_size,
                                   int timeout_ms, int* status);

// Reply to the pending ask (called from inside an actor's receive handler).
// data/data_size describe the reply payload; it is copied internally.
void scheduler_reply(ActorBase* self, void* data, size_t data_size);
/* As scheduler_reply, for a reply whose bytes own something (strings copied,
 * closures taken for the asker): when the reply is not delivered (the asker
 * timed out, nobody asked, the copy failed) `release(data)` gives those
 * back; delivered, they are the asker's with the bytes (#2528). */
void scheduler_reply_owned(ActorBase* self, void* data, size_t data_size,
                           void (*release)(void*));

// The cooperative (no-threads) runtime's dispatch: step every actor with
// messages, at most max_per_actor messages each (0 = 1024), and return how
// many were processed. A C host's event loop calls it between frames to keep
// actors running. With threads the scheduler threads run every actor, from
// the first spawn on (#3112), so it has nothing to drain and returns 0.
int aether_scheduler_poll(int max_per_actor);

// I/O event integration — register/unregister fds for non-blocking I/O dispatch
// The fd is monitored on the specified core's epoll instance. When ready,
// an MSG_IO_READY message is delivered to the actor's mailbox.
int scheduler_io_register(int core_id, int fd, void* actor, uint32_t events);
void scheduler_io_unregister(int core_id, int fd);

// The ask in hand: set by the generated step function after mailbox_receive
// from the message's _reply_slot, read by the reply.
extern AETHER_TLS void* g_current_reply_slot;

// An ask's message (aether_send_message.c, #3387): the payload copied and
// `reply_slot` in it, built by the ask itself, never handed over through a
// thread-local (a send can step its target inline, and that handler's own
// sends read it too, #3395). 0 when there was no memory for the copy: what
// the message owns is released and nothing is sent. aether_send_built sends
// a message built so, as aether_send_message routes one.
int  aether_ask_message(void* actor, void* message_data, size_t message_size,
                        void* reply_slot, Message* out);
void aether_send_built(ActorBase* actor, Message msg);

#endif
