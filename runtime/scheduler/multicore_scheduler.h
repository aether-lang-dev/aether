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

#define MAX_ACTORS_PER_CORE 10000
#define MAX_CORES 16
#define BATCH_SIZE 64  // Process up to 64 messages per batch for better throughput
#define COALESCE_THRESHOLD 512  // Drain this many messages at once for high throughput
#ifndef AETHER_IO_MAX_FDS
#define AETHER_IO_MAX_FDS 4096  // Initial I/O fd map capacity per core (grows on demand)
#endif

// Legacy compatibility - use g_aether_config instead
#define g_sched_features g_aether_config

// Reply slot for ask/reply pattern (experimental)
// Heap-allocated per ask call; freed by whoever holds the last reference (refcounted).
typedef struct {
    void*              reply_data;   // malloc'd reply payload; returned to caller, caller must free
    size_t             reply_size;   // size of reply_data
    volatile int       reply_ready;  // 1 when reply has been set
    volatile int       timed_out;    // 1 when asker has given up
    pthread_mutex_t    mutex;        // protects reply_ready / timed_out / cond
    pthread_cond_t     cond;         // signalled by scheduler_reply()
    atomic_int         refcount;     // starts at 2 (asker + actor); freed when hits 0
} ActorReplySlot;

// I/O readiness message — delivered to actor when the poller reports fd ready.
// Value must stay inside the 256-slot actor dispatch table (see codegen_actor.c).
// 255 is the top of that range and is reserved for system messages; the Aether
// message registry allocates user message IDs starting at 0 and never hands out 255.
#define MSG_IO_READY 255

// TLS pointer to the actor whose step() is currently running on this
// thread. Set by generated *_step() functions at entry; read by the
// reactor bridge (std/net/aether_actor_bridge.c) so await_io() can
// identify the calling actor without a parameter. Unlike
// g_sync_step_actor which is specific to main-thread sync mode, this
// variable is set for every step call on every thread.
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

/* A derived actor is a struct whose first bytes ARE an ActorBase: the
 * scheduler is handed a pointer to it and casts. Anything that mirrors this
 * layout by hand drifts the moment a field is added here, and the corruption
 * is silent, so the fields live in one macro and every mirror expands it. */
#define AETHER_ACTOR_BASE_FIELDS \
    atomic_int active; \
    int id; \
    Mailbox mailbox; \
    void (*step)(void*); \
    pthread_t thread; \
    int auto_process; \
    atomic_int assigned_core; \
    /* Affinity hint: core to migrate to (-1 = none) */ \
    atomic_int migrate_to; \
    /* If set, scheduler threads must not process this actor */ \
    atomic_int main_thread_only; \
    /* Lock-free same-core messaging (lazy, only for auto_process) */ \
    SPSCQueue* spsc_queue; \
    /* Non-NULL only while an ask/reply is in flight */ \
    _Atomic(ActorReplySlot*) reply_slot; \
    /* Prevents concurrent step() calls during work-steal handoff. \
     * Set (TAS) by the thread about to call step(); cleared (release) after. \
     * Work-stealing threads that find this set retry next outer iteration. */ \
    atomic_flag step_lock; \
    /* Timeout support: fire handler if no message within timeout_ns ns. \
     * 0 = no timeout. */ \
    uint64_t timeout_ns; \
    /* Timestamp when idle started; 0 = not idle */ \
    uint64_t last_activity_ns; \
    /* Panic state: set to 1 when the actor's step() unwound via aether_panic() \
     * or a caught signal. Dead actors are skipped by the scheduler and \
     * incoming messages are dropped. One-way transition; never un-set. */ \
    atomic_int dead; \
    /* Full allocation size passed to scheduler_spawn_actor. numa_free() unmaps \
     * exactly [ptr, ptr+size), so freeing a derived actor with \
     * sizeof(ActorBase) would leak the derived-struct tail under libnuma. */ \
    size_t alloc_size;

typedef struct {
    AETHER_ACTOR_BASE_FIELDS
} ActorBase;

typedef struct {
    int core_id;
    pthread_t thread;
    ActorBase** actors;
    int actor_count;
    int capacity;
    // Per-sender SPSC channels: from_queues[src] is written ONLY by core src
    // (or by the main thread when src == MAX_CORES).  Each channel is a true
    // SPSC queue, so no CAS or locks are needed on the producer side.
    LockFreeQueue from_queues[MAX_CORES + 1];
    atomic_int running;
    atomic_int work_count;  // Approximate in-flight message count (used for load reporting)
    atomic_int steal_attempts;  // Cumulative count of successful work-steal operations
    atomic_int idle_cycles;     // Track how long core has been idle
    OptimizedSpinlock actor_lock;  // Protects actors array during migration and registration

    // Per-core message counters — only written by owning core, but read
    // cross-thread by count_pending_messages(), so must be _Atomic to avoid
    // torn reads on weakly-ordered architectures (e.g. ARM64).
    _Atomic uint64_t messages_sent;      // Messages sent FROM this core
    _Atomic uint64_t messages_processed; // Messages processed ON this core
    char counter_padding[48];    // Cache line padding to prevent false sharing

    // Message coalescing buffer — amortises enqueue atomics across bursts
    struct {
        void* actors[COALESCE_THRESHOLD];
        Message messages[COALESCE_THRESHOLD];
        int count;
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

    // Per-core I/O event loop (platform-agnostic: epoll/kqueue/poll)
    AetherIoPoller io_poller;         // Platform I/O poller instance
    AetherIoEntry* io_map;            // fd → actor mapping (heap-allocated, grows on demand)
    int io_map_capacity;              // Current allocated size of io_map
    int io_registered_count;          // Number of active I/O registrations
} Scheduler;

extern Scheduler schedulers[MAX_CORES];
extern int num_cores;
extern atomic_int next_actor_id;

// Initialize scheduler with core count (autodetects hardware)
void scheduler_init(int cores);

// Initialize with explicit optimization flags
void scheduler_init_with_opts(int cores, AetherOptFlags opts);

void scheduler_start();
void scheduler_ensure_threads_running();  // Start threads if not already started (for main-thread mode transition)
void scheduler_stop();
void scheduler_wait();      // Wait for quiescence (all pending messages processed). Non-destructive.
void scheduler_shutdown();  // Wait + stop + join threads. Call once at program exit.
void scheduler_cleanup();

int scheduler_register_actor(ActorBase* actor, int preferred_core);
void scheduler_deregister_actor(ActorBase* actor);
void scheduler_send_local(ActorBase* actor, Message msg);
void scheduler_send_remote(ActorBase* actor, Message msg, int from_core);

// Batch send for main thread fan-out patterns (fork-join)
void scheduler_send_batch_start(void);
void scheduler_send_batch_add(ActorBase* actor, Message msg);
void scheduler_send_batch_flush(void);

// NUMA-aware actor lifetime (TIER 1 - always on)
ActorBase* scheduler_spawn_actor(int preferred_core, void (*step)(void*), size_t actor_size);
void scheduler_release_actor(ActorBase* actor);

// Ask/reply: send a message and block until a reply arrives or timeout.
// Returns malloc'd reply payload on success (caller must free), NULL on timeout.
void* scheduler_ask_message(ActorBase* target, void* msg_data, size_t msg_size, int timeout_ms);

// Reply to the pending ask (called from inside an actor's receive handler).
// data/data_size describe the reply payload; it is copied internally.
void scheduler_reply(ActorBase* self, void* data, size_t data_size);

// Drain pending messages for main-thread-only actors.
// Call this from C-hosted event loops (e.g. inside a render/event callback)
// to keep Aether actors alive while the main thread is occupied in C code.
// max_per_actor: max messages to process per actor per call (0 = unlimited).
// Returns total messages processed across all actors.
int aether_scheduler_poll(int max_per_actor);

// I/O event integration — register/unregister fds for non-blocking I/O dispatch
// The fd is monitored on the specified core's epoll instance. When ready,
// an MSG_IO_READY message is delivered to the actor's mailbox.
int scheduler_io_register(int core_id, int fd, void* actor, uint32_t events);
void scheduler_io_unregister(int core_id, int fd);

// Thread-local reply slot set by the send path (sender) and step function (receiver).
// g_pending_reply_slot: set before aether_send_message so the slot rides inside the Message.
// g_current_reply_slot: set by the generated step function after mailbox_receive.
extern AETHER_TLS void* g_pending_reply_slot;
extern AETHER_TLS void* g_current_reply_slot;

#endif
