// Partitioned State Machine Scheduler - Zero-Sharing Multi-core
// Strategy: Static actor-to-core assignment, no atomics, perfect cache locality

// Platform defines must come first
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include "../utils/aether_fatal.h"   /* aether_fatal_flush (#3435) */
#include <stdlib.h>
#include <string.h>
#include <limits.h>           // LONG_MAX (least-loaded placement scan)
#include "../utils/aether_thread.h"
#ifndef _WIN32
#include <sched.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#endif
#include <errno.h>
#include <time.h>
#include "multicore_scheduler.h"
#include "../utils/aether_trace.h"
#include "../utils/aether_cpu_detect.h"
#include "../utils/aether_compiler.h"
#include "../config/aether_optimization_config.h"
#include "../aether_numa.h"
#include "../actors/aether_send_buffer.h"
#include "../actors/aether_panic.h"
#include "../actors/aether_actor_inbox.h"
#include "../actors/aether_step_barrier.h"
#include "../aether_resource_caps.h"


// Forward declaration to avoid header cycle with aether_send_message.h
extern void aether_send_message(void* actor_ptr, void* message_data, size_t message_size);

// Forward declaration: I/O polling used in scheduler thread loop
static int scheduler_io_poll(Scheduler* sched, int timeout_ms);
// Forward declaration: a pending send batch goes before a send around it
// and before a wait (#3117), defined with the batch below.
static inline void batch_flush_pending(void);

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

// Branch prediction hints — provided by aether_compiler.h (included via
// multicore_scheduler.h), no local redefinition needed.

// Architecture-specific spin-wait hint — use AETHER_CPU_PAUSE() from
// aether_compiler.h which handles GCC, Clang, and MSVC correctly.
#ifndef AETHER_PAUSE
#  define AETHER_PAUSE() AETHER_CPU_PAUSE()
#endif

// MWAIT intrinsics for x86 (auto-detected at runtime)
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#elif (defined(_M_X64) || defined(_M_IX86)) && defined(_MSC_VER)
#include <intrin.h>
#define HAS_X86_INTRINSICS 1
#else
#define HAS_X86_INTRINSICS 0
#endif

#ifdef _WIN32
#include <windows.h>
// Portable wrappers for POSIX sleep/yield (not available on MINGW)
static inline void aether_usleep(unsigned int us) { Sleep(us < 1000 ? 1 : us / 1000); }
static inline void aether_sched_yield(void) { SwitchToThread(); }
#else
static inline void aether_usleep(unsigned int us) { usleep(us); }
static inline void aether_sched_yield(void) { sched_yield(); }
#endif

// Forward reference: defined below (line ~195). Needed in aether_step_safe
// for per-core death-drain accounting.
extern AETHER_TLS int current_core_id;

// Dropped-message cleanup dependencies, needed from aether_step_safe's
// death drain onward: the payload drop lives in aether_send_message.c,
// the slot decref with the ask/reply machinery below.
static void reply_slot_decref(ActorReplySlot* slot);
static void reply_slot_fail(ActorReplySlot* slot, int why);
static void release_undelivered(ActorBase* actor, Message* msg);
static void release_undelivered_at_end(ActorBase* actor, Message* msg);
/* An ended actor's death, on its own thread while it steps nothing
 * (#2974): defined with the lifecycle below. */
static void actor_finalize(ActorBase* actor);
/* The reply slot of the ask the stepping actor holds (defined with the
 * ask/reply machinery below), for a death that leaves it unanswered. */
extern AETHER_TLS void* g_current_reply_slot;
/* The ready list (#3540), defined with the scheduler loop below: an actor
 * that has work is pushed there for its core's loop to look at, and what a
 * step leaves (a message, an armed receive timeout) is read after it. */
static void ready_push(ActorBase* actor);
static void ready_after_step(ActorBase* actor);

// --------------------------------------------------------------------------
// The overflow of an actor a core runs (#3241)
//
// A relief (a core waiting in an ask, below) that finds an actor's mailbox
// full with more of its messages queued takes them, in order, into an
// overflow that belongs to the actor: its `inbox`, the unbounded FIFO an
// actor with a thread of its own takes every message in (#2598). There, as
// in the mailbox, a message is the actor's and is no longer counted in
// flight, so the actor can be moved, mailbox and overflow together, to a
// core that runs it.
//
// The overflow then holds the actor's newest messages, behind its mailbox:
// every delivery to the actor while its overflow holds anything goes behind
// it, and every step refills the mailbox from it, so each sender's messages
// keep their order on whichever core runs the actor. Like the mailbox, the
// overflow is touched only by the thread that delivers to the actor and steps
// it (its core's), and a move hands both over at once. Only a relief starts
// one; a mailbox found full elsewhere is made room in or queued, as before.
//
// The cost on the paths that do not use it: one load of `inbox` (NULL for an
// actor that never overflowed) per delivery and per step.
// --------------------------------------------------------------------------

/* The actor's overflow while it holds a message, else NULL. */
static inline ActorInbox* actor_overflow_pending(ActorBase* actor) {
    ActorInbox* ib = atomic_load_explicit(&actor->inbox, memory_order_acquire);
    if (likely(ib == NULL) || atomic_load_explicit(&ib->count, memory_order_relaxed) == 0) return NULL;
    return ib;
}

/* `msg` behind what the overflow holds. Out of memory for a list node the
 * message has no place that keeps its order, and the process stops, as an
 * out-of-memory deferred send does (overflow_append). */
static void actor_overflow_push(ActorBase* actor, ActorInbox* ib, Message msg) {
    if (likely(actor_inbox_push(ib, msg))) return;
    aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
    fprintf(stderr, "aether: out of memory queueing a message for actor %d behind its "
                    "overflow\n", actor->id);
    abort();
}

/* `msg` for `actor`, delivered by the thread that runs it: behind what its
 * overflow holds, else into its mailbox. 0 when the mailbox is full and the
 * overflow empty, which the caller handles as a full mailbox. */
static inline int deliver_owned(ActorBase* actor, Message msg) {
    ActorInbox* ib = actor_overflow_pending(actor);
    if (unlikely(ib != NULL)) {
        actor_overflow_push(actor, ib, msg);
        return 1;
    }
    return mailbox_send(&actor->mailbox, msg);
}

/* After a step: what the overflow holds moves into the mailbox, oldest
 * first, once the mailbox is down to half, so its lock is taken once per
 * several steps. While the overflow holds anything the mailbox therefore
 * never runs empty (the overflow starts behind a full mailbox), and every
 * loop that steps an actor while its mailbox has messages steps it until
 * both are drained. After the step rather than before: a step that took
 * several messages leaves no gap either. */
static void actor_overflow_refill(ActorBase* actor, ActorInbox* ib) {
    int have = atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed);
    if (have > MAILBOX_SIZE / 2 || atomic_load_explicit(&ib->count, memory_order_relaxed) == 0)
        return;
    Message moved[MAILBOX_SIZE];
    int n = actor_inbox_take(ib, moved, MAILBOX_SIZE - have);
    mailbox_send_batch(&actor->mailbox, moved, n);
}

/* Everything the overflow holds, released undelivered (the actor died);
 * returns how many, for the caller's processed credit. */
static int actor_overflow_drain(ActorBase* actor) {
    ActorInbox* ib = atomic_load_explicit(&actor->inbox, memory_order_acquire);
    if (!ib) return 0;
    int drained = 0;
    Message spill[AETHER_INBOX_RING];
    int got;
    while ((got = actor_inbox_take(ib, spill, AETHER_INBOX_RING)) > 0) {
        for (int k = 0; k < got; k++) release_undelivered_at_end(actor, &spill[k]);
        drained += got;
    }
    return drained;
}

// --------------------------------------------------------------------------
// aether_step_safe: one step of `actor` inside the panic barrier, the one
// both schedulers share (runtime/actors/aether_step_barrier.h, #2974), with
// this scheduler's parts: its death finalize, its reply slot's failure, and
// after a step the mailbox's refill from the overflow (#3241).
//
// If the handler calls aether_panic() or takes a caught signal, the barrier
// unwinds: the actor ends, its death is finalized, the death hook fires, and
// the scheduler loop keeps running; other actors are unaffected. Dead actors
// are skipped on entry: a second attempt to step a crashed actor is a no-op.
//
// The step lock is NOT released by the barrier (#2163): all its call sites
// take it before calling and clear it after, and four of them step in a
// loop under one acquisition. Cleared on the panic path, as it once was, it
// was handed away while the caller still believed it held it, the caller
// cleared it again on its way out, and two threads were soon stepping one
// actor (heap corruption, `free(): invalid pointer` in the #2083 test).
// --------------------------------------------------------------------------
static void step_fail_slot(void* slot, int why) {
    reply_slot_fail((ActorReplySlot*)slot, why);
}

static void step_after(ActorBase* actor) {
    /* The mailbox refilled from the overflow (#3241). An actor with a
     * thread of its own takes every message through its inbox, which its
     * thread empties. Then what the step left for the core to come back to
     * (#3540): a message still waiting, a receive timeout armed. */
    if (likely(!actor->auto_process)) {
        ActorInbox* behind = atomic_load_explicit(&actor->inbox, memory_order_acquire);
        if (unlikely(behind != NULL)) {
            actor_overflow_refill(actor, behind);
            // What came from the overflow is looked at again (#3540). A
            // message delivered to the mailbox brought the core already: its
            // delivery pushed it, and every look that leaves one pushes it.
            if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0)
                ready_push(actor);
        }
        if (unlikely(actor->timeout_ns != 0)) ready_after_step(actor);
    }
}

/* 1 when the step took a message, which its caller counts handled; 0 when it
 * took none: a dead actor is not stepped (its finalize credits what waits),
 * and a step of an empty mailbox fires a timeout's arm or nothing. Every
 * caller holds the step lock, so no other step takes a message meanwhile,
 * and a step of a mailbox with one takes it. Counted by the step call, a
 * call that took nothing (an actor killed mid-drain, one stolen between a
 * caller's look and its lock) credited a message never handled, and the
 * handled count passed the sent count for good, hiding as many pending
 * messages from wait_for_idle() (#3464). */
static inline int aether_step_safe(ActorBase* actor) {
    if (!actor || !actor->step) return 0;
    if (atomic_load_explicit(&actor->dead, memory_order_acquire)) return 0;
    int took = atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0;
    aether_step_barrier(actor, actor_finalize, step_fail_slot, step_after);
    return took;
}

int scheduler_step_actor(ActorBase* actor) {
    return aether_step_safe(actor);
}

/* Layout assertions to catch struct padding/size mismatches between
 * translation units. A derived actor is cast to ActorBase*, and both mirror
 * AETHER_ACTOR_BASE_FIELDS, so any disagreement about Mailbox's layout
 * silently shifts every field after it.
 *
 * Both sizes are pointer-width dependent (Message carries an intptr_t and
 * three pointers), so each width states its own expectation. The previous
 * `sizeof(Mailbox) % 8 == 0` spelling could not: on LP64 it was vacuously
 * TRUE (Mailbox contains pointers, so its size is necessarily a multiple of
 * 8) and on ILP32 vacuously FALSE (1036 % 8 == 4) — which is why every
 * 32-bit target failed to compile this TU rather than being checked by it
 * (#1652). It asserted a property no conforming ABI can violate, and the
 * only thing it ever detected was the pointer width.
 *
 * The Mailbox check below is the one the comment always claimed: the ring
 * buffer plus its three counters, with no interior padding and only natural
 * tail padding.
 *
 * It is deliberately a size check rather than a field inventory, because size
 * is exactly what the derived-actor cast depends on: the fields after
 * `mailbox` shift if and only if sizeof(Mailbox) shifts. A field small enough
 * to land in existing tail padding therefore does NOT trip it — correctly, as
 * it moves nothing. That is width-dependent: on LP64 there are 4 bytes of tail
 * padding to absorb an int, on ILP32 there are none, so the same edit fires
 * there and not here.
 *
 * AETHER_DEBUG_MAILBOX adds _send_guard and is therefore excluded. Note that
 * flag is itself the hazard this block guards against: on ILP32 it changes
 * sizeof(Mailbox) (1036 -> 1040), while on LP64 it disappears into existing
 * tail padding. Defining it for some translation units but not all corrupts
 * derived-actor layout on 32-bit targets and silently gets away with it on
 * 64-bit ones. */
#define AETHER_ROUND_UP(n, a) (((n) + (a) - 1) / (a) * (a))
#if INTPTR_MAX == INT64_MAX
_Static_assert(sizeof(Message) == 48, "Message size changed, update tests");
#elif INTPTR_MAX == INT32_MAX
_Static_assert(sizeof(Message) == 32, "Message size changed, update tests");
#endif
#ifndef AETHER_DEBUG_MAILBOX
_Static_assert(sizeof(Mailbox) == AETHER_ROUND_UP(sizeof(Message) * MAILBOX_SIZE
                                                  + 3 * sizeof(int),
                                                  _Alignof(Mailbox)),
               "Mailbox layout changed (unexpected padding or a new field)");
#endif

Scheduler schedulers[MAX_CORES];
int num_cores = 0;
atomic_int next_actor_id = 1;

// Per-core message tracking for wait_for_idle() - no atomic contention!
// Messages sent from main thread (non-scheduler threads) use this atomic counter
// This is rare (just initial messages), so the atomic overhead is negligible
static atomic_uint_fast64_t main_thread_sent = 0;

// Guard: scheduler_wait() joins threads exactly once even if called multiple times.
// Without this, calling wait_for_idle() followed by the shutdown sequence would
// pthread_join already-joined threads — undefined behaviour (crash on Linux glibc).
static atomic_int g_threads_joined = 0;

// Whether scheduler_start() has created the threads this lifecycle.
static atomic_int g_threads_started = 0;

// Barrier: scheduler_start() spins here until every thread has finished its
// setup (pin_to_core, send_buffer_init) and entered its main loop.  This
// ensures that by the time scheduler_start() returns, all scheduler threads
// are actively polling from_queues — eliminating the "thread created but not
// yet scheduled" race that causes tests to time out before any messages are
// processed.
static atomic_int g_threads_ready = 0;

// Scheduler lifecycle: 0 = not initialized, 1 = scheduler_init() running,
// 2 = initialized. A program's main() is not the only caller any more: a
// library loaded into a program that has no actors of its own initializes the
// scheduler on its first spawn (#2297). A second scheduler_init() while one is
// live must not wipe the actor tables of the actors already running, so it is
// a no-op; scheduler_shutdown() and scheduler_cleanup() end the lifecycle.
static atomic_int g_sched_state = 0;
/* #2912: sends from threads the scheduler does not own that are between
 * their check of g_sched_state and their last touch of a core. */
static atomic_int g_foreign_senders = 0;

AETHER_TLS int current_core_id = -1;
AETHER_TLS void* g_current_step_actor = NULL;
/* The core whose scheduler thread this is, -1 on every other thread
 * (#3540). current_core_id is not that: a test sets it on the main thread
 * to spawn as a core would, and a core's ready list and timer heap are its
 * thread's alone. */
static AETHER_TLS int t_core = -1;

/* The sent and handled counts scheduler_wait() compares (#3541). A core's
 * pair is written by its own thread alone, so it adds with a load and a
 * release store, not an atomic add (a lock-prefixed instruction on x86 at
 * every send and every inline step); every other thread (main, a host's, an
 * actor's own thread) adds atomically to the shared counts, which many
 * write. Only the sums are read: a message counted on one core and handled
 * on another, or handled off any core, counts the same. Release, as the
 * atomic adds were: count_pending_messages reads them with seq_cst. */
static _Atomic uint64_t g_foreign_processed = 0;

static inline void counter_add_own(_Atomic uint64_t* c, uint64_t n) {
    atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + n,
                          memory_order_release);
}

/* `n` messages sent from this thread, counted before they can be taken. */
static inline void count_sent(uint64_t n) {
    int me = t_core;
    if (likely(me >= 0)) counter_add_own(&schedulers[me].messages_sent, n);
    else atomic_fetch_add_explicit(&main_thread_sent, n, memory_order_relaxed);
}

/* `n` messages handled or dropped on this thread, after it is done with them. */
static inline void count_processed(uint64_t n) {
    int me = t_core;
    if (likely(me >= 0)) counter_add_own(&schedulers[me].messages_processed, n);
    else atomic_fetch_add_explicit(&g_foreign_processed, n, memory_order_release);
}

void scheduler_count_processed(int n) {
    if (n > 0) count_processed((uint64_t)n);
}

// ── Deferred sends (back-pressure relief) ─────────────────────────────────
// When a scheduler thread's cross-core enqueue fails after a bounded retry,
// the (actor, msg) pair is stored here instead of spin-waiting.  This lets
// the thread return to the scheduler loop, drain its own from_queues, and
// then retry the deferred sends on the next iteration — breaking the
// circular-wait that causes the queue back-pressure deadlock.
//
// One buffer per target core (indexed 0..MAX_CORES), allocated on first use.
// Per-target-core FIFO ordering is maintained: if any messages are already
// pending for target T, new sends to T are also deferred (not bypassed).

typedef struct {
    ActorBase** actors;   // heap-allocated parallel array
    Message*    msgs;     // parallel array, Message stored by value
    int         head;     // index of first valid entry (entries before head are dead)
    int         count;    // number of valid entries [head .. head+count)
    int         capacity;
} OverflowBuf;

static AETHER_TLS OverflowBuf tls_overflow[MAX_CORES + 1]; // +1 for main-thread slot
static AETHER_TLS int tls_overflow_any = 0; // fast gate: any pending at all?

// Global atomic counter: total entries across all threads' overflow buffers.
// Incremented in overflow_append, decremented in overflow_flush.
// Allows count_pending_messages (called from main thread) to detect messages
// that are still queued in scheduler threads' TLS overflow buffers.
static atomic_int g_overflow_total = 0;

// Idle park bounds (#1517). A core given work directly is signalled and does
// not wait for these; the ceiling only bounds how long a core that could STEAL
// from a busy neighbour stays asleep, and it pays that once before finding
// work and staying awake. Measured at 32ms: 0.28% of one core while idle,
// against 1.4% at 4ms, with no difference in fork-join throughput either way.
#define PARK_MS_MIN 1
#define PARK_MS_MAX 32

// How long a core with nothing to do spins before it parks (#3540): long
// enough that a message that comes a moment later (the next ask of a round
// trip, a reply on its way) finds it awake, short enough that an idle
// program parks its cores at once (#1517). About what the 10,000 passes it
// replaces took with a few actors per core on an x86 desktop (1.3 to 3 ms
// measured); a pass that read every actor took longer.
#ifndef AETHER_IDLE_SPIN_NS
#define AETHER_IDLE_SPIN_NS 2000000ULL
#endif

// A core's park waits this long instead, when a test sets it (0: the
// PARK_MS_MIN..PARK_MS_MAX steps above). Set to a minute, a wake that never
// came shows as a stall of a minute rather than as a delay of up to 32 ms,
// which no bound on wall time tells apart from a slow machine (#3087).
static atomic_int g_core_park_ms = 0;

void scheduler_set_core_park_ms(int ms) {
    atomic_store_explicit(&g_core_park_ms, ms > 0 ? ms : 0, memory_order_relaxed);
}

static inline void aether_park_deadline(struct timespec* ts, int ms) {
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, ts);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
#endif
    // In 64 bits: `long` is 32 on Windows (and on 32-bit targets), where
    // ms * 1000000 overflowed past 2147 ms and set the deadline in the past,
    // or, with a negative tv_nsec, weeks ahead. A test's minute-long park
    // returned at once, or did not return (#3095).
    long long nsec = (long long)ts->tv_nsec + (long long)ms * 1000000LL;
    ts->tv_sec += (time_t)(nsec / 1000000000LL);
    ts->tv_nsec = (long)(nsec % 1000000000LL);
}

// Tell `target` it has work (#1517); every enqueue onto another core's queues
// calls it. The mutex is touched only when someone is actually asleep.
//
// The sleeper stores `parked` and then looks at its queues; the caller has
// stored its message and then loads `parked`. Each is a store followed by a
// load of the other side's location, which a CPU may reorder (x86 does, from
// its store buffer): with only release and acquire, both sides could read the
// old value, the caller skip the signal and the core sleep on the message
// until its timed wait ran out (#3087), up to PARK_MS_MAX, and on Windows,
// whose timed waits end on its 15.6 ms timer tick, at least that. With a
// sequentially consistent fence on each side, here and in the park, at least
// one sees the other's store, as the actor-thread park below does with
// thread_parked.
static inline void sched_wake(Scheduler* target) {
    atomic_thread_fence(memory_order_seq_cst);
    // The hint the target's stepping loops read to yield to its queues
    // (#3541), after the fence: the target clears it and then fences before
    // it drains, so either this load sees the clear and sets it again, or
    // the drain sees the message. A store only when it reads 0, so a busy
    // target's line is not written on every send.
    if (!atomic_load_explicit(&target->inbound, memory_order_relaxed))
        atomic_store_explicit(&target->inbound, 1, memory_order_relaxed);
    if (atomic_load_explicit(&target->parked, memory_order_relaxed)) {
        pthread_mutex_lock(&target->park_mutex);
        pthread_cond_signal(&target->park_cond);
        pthread_mutex_unlock(&target->park_mutex);
    }
}

// ---------------------------------------------------------------------------
// Messages in flight, and moving an actor between cores (#3090, #3093)
//
// A message one actor sends another goes either straight into the target's
// mailbox, by the target's own core (a local send), or through a core's
// queues: a from_queue, or a sender's overflow buffer on its way to one.
// Moving the target to another core while a message of the second kind was
// on its way left that message behind: the old core forwarded it, while the
// sender's next message went to the new core directly and could be stepped
// first (#3093, 31k of 3.2M messages out of order in a test).
//
// So every message routed through the queues is counted in the target's
// `inflight` before the sender reads assigned_core (inflight_enter), and
// uncounted when it reaches the mailbox, or the overflow behind it (#3241),
// either of which moves with the actor (inflight_leave). An actor moves
// only by its own core, at a point of that core's loop where it is stepping
// nothing (so no local send to it is under way), or from the relief of that
// core waiting in an ask (#3197), where the step under way is blocked in the
// ask with no send under way either, and only after
// move_begin() has turned `inflight` from 0 to AETHER_INFLIGHT_MOVING: no
// message for it is anywhere but its mailbox, which moves with it. A sender
// that counts its message during the move waits for move_end() before it
// reads assigned_core, and then reads the new core.
// ---------------------------------------------------------------------------

static inline int move_begin(ActorBase* actor) {
    int none = 0;
    return atomic_compare_exchange_strong_explicit(&actor->inflight, &none,
                                                   AETHER_INFLIGHT_MOVING,
                                                   memory_order_acq_rel,
                                                   memory_order_relaxed);
}

static inline void move_end(ActorBase* actor) {
    atomic_fetch_and_explicit(&actor->inflight, ~AETHER_INFLIGHT_MOVING, memory_order_release);
}

// ---------------------------------------------------------------------------
// Idle park of actor threads (#2592)
//
// An auto_process actor's thread (aether_actor_thread) that has spun a while
// with nothing to do sleeps on its core's condition below. A message wakes
// it (every send to it goes through scheduler_actor_thread_deliver), and so
// do its release, the scheduler's stop and its teardown. One condition
// serves every such thread of a core and is broadcast, since a wake is for
// one actor; the others look at their inboxes and sleep again. The wait is
// timed as well, so a wake that is missed costs latency, never a hang.
//
// The conditions are made once per process and never again. A core's own
// park_mutex lives for one lifecycle (made by scheduler_init(), destroyed
// with the core tables once its thread was joined); an actor thread is not
// joined by the scheduler and may still be leaving the park when the next
// scheduler_init() runs.
// ---------------------------------------------------------------------------

static pthread_mutex_t g_actor_park_mutex[MAX_CORES];
static pthread_cond_t  g_actor_park_cond[MAX_CORES];
static atomic_int g_actor_park_state = 0;   // 0 not made, 1 making, 2 ready
static atomic_int g_actor_park_ms = PARK_MS_MAX;

void scheduler_set_actor_park_ms(int ms) {
    atomic_store_explicit(&g_actor_park_ms, ms > 0 ? ms : PARK_MS_MAX, memory_order_relaxed);
}

static void actor_park_init_once(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_actor_park_state, &expected, 1)) {
        for (int i = 0; i < MAX_CORES; i++) {
            pthread_mutex_init(&g_actor_park_mutex[i], NULL);
            pthread_cond_init(&g_actor_park_cond[i], NULL);
        }
        atomic_store_explicit(&g_actor_park_state, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&g_actor_park_state, memory_order_acquire) != 2) {
            AETHER_PAUSE();
        }
    }
}

// The core an actor thread parks on, or -1. Such an actor stays on the core
// it was registered on: no core steps it, so neither migration nor work
// stealing moves it.
static inline int actor_park_core(ActorBase* actor) {
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
    return (core >= 0 && core < MAX_CORES) ? core : -1;
}

// Wakes every actor thread parked on `core`. Takes the mutex, so a thread
// that has not reached its wait yet is past its last look first, and that
// look sees whatever the caller set before calling.
static void actor_park_broadcast(int core) {
    if (core < 0 ||
        atomic_load_explicit(&g_actor_park_state, memory_order_acquire) != 2) {
        return;
    }
    pthread_mutex_lock(&g_actor_park_mutex[core]);
    pthread_cond_broadcast(&g_actor_park_cond[core]);
    pthread_mutex_unlock(&g_actor_park_mutex[core]);
}

static void actor_thread_wake(ActorBase* actor) {
    // Pairs with the fence in scheduler_actor_thread_park. The message the
    // caller has just put in the inbox is seen by the thread's last look at
    // it, or this load sees the thread parked, or both: without the two
    // fences each side could miss the other's store, and the thread would
    // sleep on a message for a whole timed wait.
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&actor->thread_parked, memory_order_relaxed)) {
        actor_park_broadcast(actor_park_core(actor));
    }
}

int scheduler_actor_thread_park(ActorBase* actor) {
    int core = actor_park_core(actor);
    if (core < 0 ||
        atomic_load_explicit(&g_actor_park_state, memory_order_acquire) != 2) {
        return 0;
    }
    // Asleep, the thread holds no actor and reads no table, so it stops
    // being a reader: otherwise the epoch it last published would hold every
    // release back for as long as it sleeps.
    scheduler_reader_offline();
    pthread_mutex_lock(&g_actor_park_mutex[core]);
    atomic_store_explicit(&actor->thread_parked, 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    // The last look, after `thread_parked` is published. A release or a stop
    // sets its mark and then broadcasts under this mutex, so the mark is seen
    // here or the broadcast ends the wait below.
    ActorInbox* ib = atomic_load_explicit(&actor->inbox, memory_order_acquire);
    if (!atomic_load_explicit(&actor->dead, memory_order_acquire) &&
        atomic_load_explicit(&schedulers[core].running, memory_order_acquire) &&
        atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) == 0 &&
        (!ib || atomic_load_explicit(&ib->count, memory_order_acquire) == 0)) {
        struct timespec deadline;
        aether_park_deadline(&deadline,
                             atomic_load_explicit(&g_actor_park_ms, memory_order_relaxed));
        pthread_cond_timedwait(&g_actor_park_cond[core], &g_actor_park_mutex[core],
                               &deadline);
    }
    atomic_store_explicit(&actor->thread_parked, 0, memory_order_relaxed);
    pthread_mutex_unlock(&g_actor_park_mutex[core]);
    scheduler_reader_online();
    return 1;
}

// The actor's inbox, made by the first send. Two first sends at once each
// make one; the loser frees its own.
static ActorInbox* actor_inbox_of(ActorBase* actor) {
    ActorInbox* ib = atomic_load_explicit(&actor->inbox, memory_order_acquire);
    if (likely(ib != NULL)) return ib;
    ActorInbox* fresh = actor_inbox_new();
    if (!fresh) return NULL;
    ActorInbox* expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&actor->inbox, &expected, fresh,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        return fresh;
    }
    free(fresh);
    return expected;
}

void scheduler_actor_thread_deliver(ActorBase* actor, Message msg) {
    AETHER_TRACE_EVENT(AE_TRACE_SPSC_ENQUEUE, actor->id, msg.type, msg.sender_id);
    ActorInbox* ib = actor_inbox_of(actor);
    if (unlikely(!ib || !actor_inbox_push(ib, msg))) {
        // Out of memory: the message cannot be kept. It was counted as
        // sent, so it is released and credited as processed, or
        // scheduler_wait() would wait for it forever.
        fprintf(stderr, "aether: out of memory queueing a message for actor %d; "
                        "it is dropped\n", actor->id);
        release_undelivered(actor, &msg);
        count_processed(1);
        return;
    }
    atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    actor_thread_wake(actor);   // its seq_cst fence orders the push before the load below
    /* The actor ended (#3298): its thread drains its inbox as it finalizes
     * the death (actor_finalize, which fences after the death bit), and a
     * message pushed after that drain would wait there uncredited, holding
     * scheduler_wait up. One of the two sees the other: this sender drops
     * what is left. A released actor's inbox goes with its block. */
    int ended = atomic_load_explicit(&actor->dead, memory_order_acquire);
    if (unlikely(ended & AETHER_ACTOR_ENDED) && !(ended & AETHER_ACTOR_RELEASED) &&
        (atomic_load_explicit(&actor->exit_flags, memory_order_acquire) & AETHER_EXIT_FINALIZED)) {
        int dropped = actor_overflow_drain(actor);
        if (dropped > 0) {
            count_processed((uint64_t)dropped);
            scheduler_count_dead_sends(dropped);
        }
    }
}

// ---------------------------------------------------------------------------
// The end of an actor (#2974)
//
// An actor ends once, by the first of its death bits (aether_actor_claim_end):
// a panic in its step, a kill or a linked actor's death (KILLED), its own
// stop() (STOPPED), or a release. The death is finalized on the thread that
// owns the actor's mailbox, while the actor steps nothing: what waits for it
// is dropped and credited, and its watchers are told (aether_watch_notify),
// so no Down is sent while its handler still runs and each sender's last
// messages before it arrive first. A panic is finalized in the step barrier,
// a stop() and an end that came during a step after the step, an actor
// thread's death at the end of its thread's loop, and an end from elsewhere
// to an actor not stepping by its core, through the core's control requests.
// A release is its own path: its watchers are told at the release and its
// messages dropped when its block is reclaimed.
// ---------------------------------------------------------------------------

static _Atomic uint64_t g_dead_sends = 0;

uint64_t scheduler_dead_sends(void) {
    return atomic_load_explicit(&g_dead_sends, memory_order_relaxed);
}

void scheduler_count_dead_sends(int n) {
    if (n > 0) atomic_fetch_add_explicit(&g_dead_sends, (uint64_t)n, memory_order_relaxed);
}

static void actor_finalize(ActorBase* actor) {
    if (atomic_fetch_or_explicit(&actor->exit_flags, AETHER_EXIT_FINALIZED, memory_order_acq_rel) &
        AETHER_EXIT_FINALIZED)
        return;
    // Pairs with a sender's fence after its push into an actor thread's
    // inbox (scheduler_actor_thread_deliver, #3298).
    atomic_thread_fence(memory_order_seq_cst);
    int drained = 0;
    Message discard;
    while (mailbox_receive(&actor->mailbox, &discard)) {
        release_undelivered_at_end(actor, &discard);
        drained++;
    }
    // Its overflow (#3241), or an actor thread's inbox (#3298).
    drained += actor_overflow_drain(actor);
    if (drained > 0) {
        count_processed((uint64_t)drained);
        scheduler_count_dead_sends(drained);
    }
    const char* reason = aether_actor_death_reason(actor);
    aether_watch_notify(actor, reason ? reason : "normal");
}

void scheduler_actor_thread_finalize(ActorBase* actor) {
    int d = atomic_load_explicit(&actor->dead, memory_order_acquire);
    if ((d & AETHER_ACTOR_ENDED) && !(d & AETHER_ACTOR_RELEASED)) actor_finalize(actor);
}

/* A finalize request for `actor`, to core `core`'s loop. 0 when that loop
 * takes no more (its thread has left it): the caller finalizes the death
 * itself, as nothing steps the actor any more. */
static int control_post(int core, ActorBase* actor) {
    Scheduler* s = &schedulers[core];
    while (atomic_flag_test_and_set_explicit(&s->control_lock, memory_order_acquire)) AETHER_PAUSE();
    if (s->control_closed) {
        atomic_flag_clear_explicit(&s->control_lock, memory_order_release);
        return 0;
    }
    if (s->control_count == s->control_cap) {
        int cap = s->control_cap ? s->control_cap * 2 : 8;
        ActorBase** grown = (ActorBase**)realloc(s->control, (size_t)cap * sizeof(ActorBase*));
        if (!grown) {
            atomic_flag_clear_explicit(&s->control_lock, memory_order_release);
            fprintf(stderr, "aether: out of memory ending actor %d; it ends at its next step\n",
                    actor->id);
            return 1;
        }
        s->control = grown;
        s->control_cap = cap;
    }
    s->control[s->control_count++] = actor;
    // Counted until the request is done, for scheduler_wait: the Downs
    // its finalize sends are not sent yet.
    atomic_fetch_add_explicit(&s->control_pending, 1, memory_order_release);
    atomic_flag_clear_explicit(&s->control_lock, memory_order_release);
    sched_wake(s);
    return 1;
}

/* The core's finalize requests, on its thread where it steps nothing, or
 * from its relief: an actor stepping lower on this thread is finalized by
 * its own step's end instead. One that moved is passed to its core. */
static void control_run(Scheduler* sched) {
    if (likely(!atomic_load_explicit(&sched->control_pending, memory_order_acquire))) return;
    while (atomic_flag_test_and_set_explicit(&sched->control_lock, memory_order_acquire)) AETHER_PAUSE();
    ActorBase** list = sched->control;
    int n = sched->control_count;
    sched->control = NULL;
    sched->control_count = 0;
    sched->control_cap = 0;
    atomic_flag_clear_explicit(&sched->control_lock, memory_order_release);
    for (int i = 0; i < n; i++) {
        ActorBase* actor = list[i];
        int d = atomic_load_explicit(&actor->dead, memory_order_acquire);
        int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
        if (!(d & AETHER_ACTOR_ENDED) || (d & AETHER_ACTOR_RELEASED) ||
            (atomic_load_explicit(&actor->exit_flags, memory_order_acquire) & AETHER_EXIT_FINALIZED)) {
            // Nothing to do.
        } else if (core != sched->core_id && core >= 0 && core < num_cores &&
                   control_post(core, actor)) {
            // Its core's now. One that takes no more requests has left its
            // loop and steps nothing: finalized here, below.
        } else if (!atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
            // Read again under the lock: a restart in place may have
            // brought it alive meanwhile (#2974).
            d = atomic_load_explicit(&actor->dead, memory_order_acquire);
            if ((d & AETHER_ACTOR_ENDED) && !(d & AETHER_ACTOR_RELEASED)) actor_finalize(actor);
            atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
        }
        // else stepping lower on this thread: its step's end finalizes it.
        atomic_fetch_sub_explicit(&sched->control_pending, 1, memory_order_release);
    }
    free(list);
}

/* An ask the ended actor is waiting in, as its core's innermost ask, ends at
 * once (its wait checks the bit when woken). Read under the core's lock,
 * under which the asker puts the slot back before it lets it go. */
static void wake_ended_asker(ActorBase* actor) {
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    if (core < 0 || core >= num_cores) return;
    Scheduler* s = &schedulers[core];
    while (atomic_flag_test_and_set_explicit(&s->actor_lock.lock, memory_order_acquire)) AETHER_PAUSE();
    if (atomic_load_explicit(&s->asking, memory_order_acquire) > 0 &&
        atomic_load_explicit(&s->ask_asker, memory_order_acquire) == actor) {
        ActorReplySlot* slot = atomic_load_explicit(&s->ask_slot, memory_order_acquire);
        if (slot) {
            pthread_mutex_lock(&slot->mutex);
            pthread_cond_signal(&slot->cond);
            pthread_mutex_unlock(&slot->mutex);
        }
    }
    atomic_flag_clear_explicit(&s->actor_lock.lock, memory_order_release);
}

/* A reborn actor is alive (#2974): its core is told through its ready list
 * (#3540), and puts a receive timeout of the new incarnation in its heap. */
void scheduler_actor_revived(ActorBase* actor) {
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    if (core >= 0 && core < num_cores) ready_push(actor);
}

int scheduler_actor_end(ActorBase* actor, const char* reason, int bit) {
    if (!actor || !aether_actor_claim_end(actor, reason, bit)) return 0;
    if (actor->auto_process) {
        // Its thread leaves its loop at the bit and finalizes the death
        // there; asleep in its park, it is woken. Gone already, nothing else
        // takes its mailbox or inbox, and the death is finalized here.
        if (atomic_load_explicit(&actor->dead, memory_order_acquire) & AETHER_ACTOR_THREAD_GONE)
            actor_finalize(actor);
        else
            actor_park_broadcast(actor_park_core(actor));
        return 1;
    }
    wake_ended_asker(actor);
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    /* #3399: posted only where a core thread will take the request, once
     * the scheduler is initialized and its threads started. Initialized
     * is not running: scheduler_init sets the state before
     * scheduler_start makes the threads, and a request posted between the
     * two waited for a start that a C host may never make, so the death
     * was never finalized, no Down went out and no link took it. */
    /* And while the cores are stopping (scheduler_shutdown has left the
     * running state, but a core thread may still be in a step of the actor,
     * a receive timeout's arm say, which the shutdown does not wait for):
     * finalized here, it was drained and its watchers told while its
     * handler ran on its core. Posted, the core finalizes it when the step
     * returns, or on its way out of its loop; a core that has left its loop
     * refuses the request, and nothing steps the actor any more. */
    if (core >= 0 && core < num_cores &&
        atomic_load_explicit(&g_threads_started, memory_order_acquire) &&
        control_post(core, actor)) {
        return 1;
    }
    // No core runs it (the scheduler is not running, its threads are not
    // started, or its core has left its loop): nothing steps it or delivers
    // to it, so it is finalized here.
    actor_finalize(actor);
    return 1;
}

static void overflow_append(int target, ActorBase* actor, Message msg) {
    if (unlikely(target < 0 || target > MAX_CORES)) {
        fprintf(stderr, "aether: overflow_append: target %d out of range [0, %d]\n",
                target, MAX_CORES);
        return;
    }
    OverflowBuf* b = &tls_overflow[target];
    int tail = b->head + b->count;
    if (unlikely(tail >= b->capacity)) {
        // Compact: move valid entries to front before growing
        if (b->head > 0) {
            memmove(b->actors, b->actors + b->head, (size_t)b->count * sizeof(ActorBase*));
            memmove(b->msgs,   b->msgs   + b->head, (size_t)b->count * sizeof(Message));
            b->head = 0;
            tail = b->count;
        }
        if (tail >= b->capacity) {
            int nc = b->capacity ? b->capacity * 2 : 64;
            while (nc <= tail) nc *= 2;
            ActorBase** new_actors = realloc(b->actors, (size_t)nc * sizeof(ActorBase*));
            if (unlikely(!new_actors)) {
                aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
                fprintf(stderr, "aether: OOM in overflow_append (target=%d count=%d cap=%d)\n",
                        target, b->count, b->capacity);
                abort();
            }
            b->actors = new_actors;
            Message* new_msgs = realloc(b->msgs, (size_t)nc * sizeof(Message));
            if (unlikely(!new_msgs)) {
                aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
                fprintf(stderr, "aether: OOM in overflow_append (target=%d count=%d cap=%d)\n",
                        target, b->count, b->capacity);
                abort();
            }
            b->msgs = new_msgs;
            b->capacity = nc;
        }
    }
    b->actors[tail] = actor;
    b->msgs[tail]   = msg;
    b->count++;
    tls_overflow_any = 1;
    atomic_fetch_add_explicit(&g_overflow_total, 1, memory_order_relaxed);
}

// The overflow buffers are this thread's own (thread-local), and they keep
// what they grew to, so a burst is not paid for again. A scheduler thread
// gives them back when it leaves its loop: nothing else can reach them once
// the thread is gone, and a pair of actors trading messages at full speed
// grew them to 15 to 28 MB per core, all of it lost at every scheduler
// shutdown. An entry still in one when the thread leaves (its final flush
// found the target's queue full, with the cores stopping) is dropped and
// uncounted, as a message for a released actor is.
static void scheduler_drop_dead_message(Scheduler* sched, ActorBase* actor, Message* msg);

static void overflow_release(Scheduler* sched) {
    int dropped = 0;
    for (int t = 0; t <= MAX_CORES; t++) {
        OverflowBuf* b = &tls_overflow[t];
        for (int i = b->head; i < b->head + b->count; i++) {
            ActorBase* actor = b->actors[i];
            int counted = actor && !actor->auto_process;
            // Not a death's drop: the cores are stopping.
            release_undelivered(actor, &b->msgs[i]);
            if (counted) inflight_leave(actor, 1);
            count_processed(1);
            dropped++;
        }
        free(b->actors);
        free(b->msgs);
        b->actors = NULL;
        b->msgs = NULL;
        b->head = 0;
        b->count = 0;
        b->capacity = 0;
    }
    if (dropped > 0) atomic_fetch_sub_explicit(&g_overflow_total, dropped, memory_order_relaxed);
    tls_overflow_any = 0;
}

// Queue a counted message for one of this core's own actors on this core's
// own queue, behind whatever this core's own overflow buffer holds: an entry
// there overflowed when the queue was full, so it is older, and a message
// put in the queue past it was stepped first (#3093). send_through_core
// keeps the same order for another core's queue.
static void requeue_own(int core, ActorBase* actor, Message msg) {
    if (tls_overflow[core].count > 0 ||
        !queue_enqueue(&schedulers[core].from_queues[core], actor, msg)) {
        overflow_append(core, actor, msg);
    }
    // The loop yields to its own queue as to the others (#3541). This
    // thread's own core: a plain store.
    atomic_store_explicit(&schedulers[core].inbound, 1, memory_order_relaxed);
}

/* Whether a message came into this core's queues since it last drained them
 * (#3541): what its stepping loops yield to. One load of a word on the line
 * `parked` is on, where has_pending_cross_core loads the head and tail of
 * every queue of the core. */
static inline int inbound_pending(Scheduler* sched) {
    return atomic_load_explicit(&sched->inbound, memory_order_relaxed) != 0;
}

// Try to flush deferred sends into their target from_queues.
// Called at the top of each scheduler_thread iteration.
//
// Two modes for own-core overflow:
// - SMALL (≤4096): Direct mailbox delivery + step() — fast path for normal
//   workloads (ping-pong, thread-ring) where overflow is transient.
// - LARGE (>4096): Queue-based drain (same as cross-core) — prevents the
//   20:1 amplification cascade in tree-spawning workloads (skynet) where
//   each step() generates many new cross-core sends.
//
// Cross-core: Always uses head-pointer advancement instead of memmove.
// This makes the cost O(drained) instead of O(total), eliminating the
// multi-MB memmoves that caused phase-2 stalls with large overflow.
//
// `own` 0 is a relief's (#3197): a step is under way on this thread and the
// relief steps nothing, so this core's own buffer goes through its own
// queue, as a large one does, never through the path that steps what it
// delivers.
// Set while a flush runs on this thread. Its own-core path steps actors
// before it takes their entries off the buffer, and a step that flushes in
// turn (an inline send, a relief) would deliver those entries again and
// leave the outer loop counting from a buffer it changed (#3247): the inner
// call leaves the flush to the one under way.
static AETHER_TLS int tls_overflow_flushing = 0;

static void overflow_flush_ex(int from_core, int own) {
    if (!tls_overflow_any || tls_overflow_flushing) return;
    tls_overflow_flushing = 1;
    int from_idx = (from_core >= 0 && from_core < MAX_CORES) ? from_core : MAX_CORES;
    int any = 0;
    int flushed = 0;
    int direct_processed = 0;
    for (int t = 0; t < num_cores; t++) {
        OverflowBuf* b = &tls_overflow[t];
        if (b->count == 0) continue;

        // ── OWN-CORE (small overflow): direct mailbox + step() ─────────
        // Bypass the from_queue entirely for low-contention fast path.
        // When overflow is large (tree-spawn cascade), fall through to the
        // uniform queue-based path below to avoid amplification. Not while
        // this core's own queue holds messages: an entry overflowed because
        // that queue was full, so the queue's are older, and delivered past
        // them it was stepped first (#3093: 511 of a sender's messages
        // overtaken by its next 128).
        if (own && t == from_core && from_core >= 0 && b->count <= 4096 &&
            queue_size(&schedulers[from_core].from_queues[from_core]) == 0) {
            int start = b->head;
            int orig_count = b->count;
            int limit = orig_count < 128 ? orig_count : 128;
            int rem = 0;  // compacted "keep" entries [start..start+rem)
            for (int i = 0; i < limit; i++) {
                ActorBase* actor = b->actors[start + i];
                Message msg = b->msgs[start + i];
                // Acquire, as in send_local_admitted: the mailbox is written below
                // when this core has the actor, after another core may have had it.
                int ac = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
                if (unlikely(atomic_load_explicit(&actor->dead, memory_order_acquire))) {
                    // Released since it was sent: dropped and credited, as the
                    // core loop drops one (#1301), and uncounted (#3090). Put in
                    // the mailbox of a released actor, it was never processed,
                    // and scheduler_wait() waited for it.
                    int counted = !actor->auto_process;
                    scheduler_drop_dead_message(&schedulers[from_core], actor, &msg);
                    if (counted) inflight_leave(actor, 1);   // after the drop, which reads it
                    flushed++;
                } else if (ac == from_core && !actor->auto_process) {
                    if (deliver_owned(actor, msg)) {
                        inflight_leave(actor, 1);
                        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
                        flushed++;
                        // Immediately process if step_lock available
                        if (actor->step &&
                            !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
                            int p = 0, took = 0;
                            while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 &&
                                   p < 16 &&
                                   atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == from_core) {
                                took += aether_step_safe(actor);
                                p++;
                            }
                            atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
                            direct_processed += took;
                        }
                        // Left with messages: the loop's ready list (#3540).
                        if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0)
                            ready_push(actor);
                    } else {
                        // Mailbox full — keep in overflow
                        b->actors[start + rem] = actor;
                        b->msgs[start + rem]   = msg;
                        rem++;
                    }
                } else if (ac >= 0 && ac < num_cores) {
                    // Actor migrated away — try to forward
                    if (queue_enqueue(&schedulers[ac].from_queues[from_idx], actor, msg)) {
                        sched_wake(&schedulers[ac]);
                        flushed++;
                    } else {
                        b->actors[start + rem] = actor;
                        b->msgs[start + rem]   = msg;
                        rem++;
                    }
                } else {
                    b->actors[start + rem] = actor;
                    b->msgs[start + rem]   = msg;
                    rem++;
                }
            }
            // Tail: entries beyond 'limit' that we didn't examine, PLUS any
            // new entries appended by step() during the loop (b->count may
            // now exceed orig_count).  Use memmove — regions may overlap
            // when rem < limit.
            int tail_count = b->count - limit;  // includes unexamined + newly added
            if (tail_count > 0 && rem < limit) {
                memmove(b->actors + start + rem, b->actors + start + limit, (size_t)tail_count * sizeof(ActorBase*));
                memmove(b->msgs   + start + rem, b->msgs   + start + limit, (size_t)tail_count * sizeof(Message));
            }
            b->count = rem + tail_count;
            // head stays at 'start' (kept entries are at [start..start+rem))
            if (b->count) any = 1;
            continue;
        }

        // ── QUEUE-BASED DRAIN (cross-core + own-core large overflow) ───
        // Enqueue entries into the target core's from_queue.  For own-core,
        // use the self-channel (from_queues[from_idx]).  Advance head pointer
        // instead of memmove — O(drained) not O(total).
        {
            LockFreeQueue* q = &schedulers[t].from_queues[from_idx];
            int start = b->head;
            int end = start + b->count;
            int i = start;
            for (; i < end; i++) {
                if (!queue_enqueue(q, b->actors[i], b->msgs[i])) {
                    break;  // Queue full — stop trying
                }
                flushed++;
            }
            int drained = i - start;
            b->head += drained;
            b->count -= drained;
            if (b->count > 0) any = 1;
            // Another core is told, as every other enqueue tells it
            // (#3087): asleep, it would sleep on these until a timeout.
            if (drained > 0 && t != from_core) sched_wake(&schedulers[t]);
            else if (drained > 0) atomic_store_explicit(&schedulers[t].inbound, 1, memory_order_relaxed);
        }
    }
    if (flushed > 0) {
        atomic_fetch_sub_explicit(&g_overflow_total, flushed, memory_order_relaxed);
    }
    if (direct_processed > 0 && from_core >= 0) {
        count_processed((uint64_t)direct_processed);
    }
    /* From the buffers, not from `any`: a step in the own-core path may have
     * appended to a buffer the loop had already passed, and a flag cleared
     * over it hid the entry from every later flush (#3247). */
    int left = any;
    for (int t = 0; t < num_cores && !left; t++) left = tls_overflow[t].count > 0;
    tls_overflow_any = left;
    tls_overflow_flushing = 0;
}

static void overflow_flush(int from_core) {
    overflow_flush_ex(from_core, 1);
}

// The CPUs this process may run on, in ascending order, read by the thread
// that starts the scheduler before any scheduler thread places itself
// (#3106). Scheduler thread i goes to the (i mod n)-th of them. It went to
// CPU i, a number from 0: under `taskset -c 4-7` a thread bound itself to
// CPU 0, outside the CPUs the program was started on (setting a thread's
// own affinity needs no privilege), and in a cpuset cgroup a CPU outside
// the set refused the binding, so placement depended on CPU numbers. 0
// CPUs (the mask could not be read) leaves every thread where the OS puts
// it.
#define AETHER_PLACE_MAX_CPUS 1024
static int g_place_cpus[AETHER_PLACE_MAX_CPUS];
static int g_place_count = 0;

static void read_process_cpus(void) {
    g_place_count = 0;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE && g_place_count < AETHER_PLACE_MAX_CPUS; c++)
            if (CPU_ISSET(c, &set)) g_place_cpus[g_place_count++] = c;
    }
#elif defined(_WIN32)
    // The process mask covers its processor group, numbered as
    // SetThreadIdealProcessor numbers them.
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask)) {
        for (int c = 0; c < (int)(sizeof(DWORD_PTR) * 8); c++)
            if (proc_mask & ((DWORD_PTR)1 << c)) g_place_cpus[g_place_count++] = c;
    }
#endif
}

// The CPU scheduler thread `core_id` is placed on, or -1 for none.
int scheduler_core_cpu(int core_id) {
    if (core_id < 0 || g_place_count <= 0) return -1;
    return g_place_cpus[core_id % g_place_count];
}

// Keep a scheduler thread on its CPU (NUMA awareness): bound to it on Linux,
// a hint on macOS and Windows.
//
// Linux keeps the hard binding (#3089). Measured on a 4-CPU CI runner
// (kernel 6.17) against the same threads unbound, under busy loops: bound
// won 7 of the 12 loaded cases (thread ring 10.8 against 7.4 M msg/s beside
// two loops the kernel placed, skynet up to 2.4x), lost only where another
// program's thread was pinned to a scheduler thread's own CPU (1.1x to
// 1.5x), and tied on a quiet machine. Linux moves movable load off a bound
// thread's CPU; Windows did not, which is why #3087 made it a hint there.
static void pin_to_core(int core_id) {
#ifdef __linux__
    int cpu = scheduler_core_cpu(core_id);
    if (cpu < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
#if defined(__ANDROID__)
    // bionic has no pthread_setaffinity_np; sched_setaffinity on the calling
    // thread's own id (0) is the same request.
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#else
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
#elif defined(__APPLE__)
    // macOS uses thread affinity tags - threads with same tag tend to run on same core
    // This is a hint to the scheduler, not a hard binding (macOS design philosophy)
    thread_affinity_policy_data_t policy = { core_id + 1 };  // Tag must be > 0
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy,
                      THREAD_AFFINITY_POLICY_COUNT);

    // Set high QoS to encourage P-core usage on Apple Silicon
    // QOS_CLASS_USER_INTERACTIVE has highest priority and prefers performance cores
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(_WIN32)
    // The ideal processor, a hint, not SetThreadAffinityMask's hard binding
    // (#3087). Bound to one logical CPU, a scheduler thread could not leave
    // it when another program's thread ran there (an editor's indexer, a
    // browser), and shared it a time slice at a time: a thread ring whose
    // path crossed such a CPU ran at a quarter of its speed, so runs fell
    // into a fast or a slow mode by where the other load landed. With the
    // hint Windows keeps the thread on its CPU and moves it when that CPU is
    // busy, as macOS's affinity tag above lets it.
    int cpu = scheduler_core_cpu(core_id);
    if (cpu >= 0) SetThreadIdealProcessor(GetCurrentThread(), (DWORD)cpu);
#endif
}

// ---------------------------------------------------------------------------
// Per-core actor table (#2486)
//
// Every writer (registration, deregistration, migration, work stealing) holds
// the core's actor_lock. Readers do not: the owning scheduler thread scans its
// table whenever it is idle, and the main thread walks every table in
// aether_scheduler_poll() and scheduler_wait(). So each word a reader looks at
// is atomic, and the orderings carry this protocol:
//
//  - A slot is stored with release and loaded with acquire. A reader that
//    finds an actor pointer also sees that actor's initialization, including
//    when the pointer reached the slot through a later swap-remove rather than
//    the append that published it.
//
//  - A table is published (release) before any count that needs it, and a
//    reader loads the count (acquire) BEFORE the table (acquire). The count it
//    reads was stored after a table large enough to hold it was published, so
//    the table it loads next is that one or a newer one, and tables only grow:
//    the count never exceeds the capacity of the table it is paired with. The
//    other order can pair a new count with the old table and index past its
//    end.
//
//  - A table that is grown out of is not freed: a reader may still be walking
//    it. It is chained on `retired` and freed by scheduler_cleanup(), after the
//    threads are joined. Each table doubles the one before, so the chain costs
//    less than the live table.
//
//  - Every slot at or past actor_count is NULL. A new table is zeroed past the
//    prefix copied into it, and a removal clears the slot it vacates, so a
//    reader holding an older, larger count finds NULL there and skips it.
// ---------------------------------------------------------------------------

// A table of `capacity` empty slots. aether_numa_alloc() is plain malloc()
// without libnuma, so the slots start as garbage until set here.
static AetherActorTable* actor_table_alloc(int capacity, int numa_node) {
    size_t size = sizeof(AetherActorTable) + (size_t)capacity * sizeof(_Atomic(ActorBase*));
    AetherActorTable* table = aether_numa_alloc(size, numa_node);
    if (!table) return NULL;
    table->retired = NULL;
    table->alloc_size = size;
    table->capacity = capacity;
    for (int i = 0; i < capacity; i++) atomic_init(&table->slots[i], NULL);
    return table;
}

// Frees a table and every table it replaced. Only once no reader is left.
static void actor_table_free_chain(AetherActorTable* table) {
    while (table) {
        AetherActorTable* older = table->retired;
        aether_numa_free(table, table->alloc_size);
        table = older;
    }
}

// The view of a core's actors for a reader that does not hold actor_lock:
// returns the table, with the number of slots to visit in *count. The count is
// loaded first; see the notes above.
static inline AetherActorTable* actor_table_snapshot(Scheduler* sched, int* count) {
    *count = atomic_load_explicit(&sched->actor_count, memory_order_acquire);
    return atomic_load_explicit(&sched->actor_table, memory_order_acquire);
}

static inline ActorBase* actor_table_read(AetherActorTable* table, int i) {
    return atomic_load_explicit(&table->slots[i], memory_order_acquire);
}

// The *_locked helpers below are for a caller holding sched->actor_lock, which
// orders them against each other; their loads are relaxed for that reason.
// Their stores are release, for the readers that do not take the lock.

static inline int actor_table_has_room_locked(Scheduler* sched) {
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    return atomic_load_explicit(&sched->actor_count, memory_order_relaxed) < table->capacity;
}

// Index of `actor` in the table, or -1 when it is not there. `hint` is the
// slot the caller last saw it in (-1 for none), checked before the scan.
static int actor_table_find_locked(Scheduler* sched, ActorBase* actor, int hint) {
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);
    if (!table) return -1;   // freed by teardown: nothing is registered
    if (hint >= 0 && hint < count &&
        atomic_load_explicit(&table->slots[hint], memory_order_relaxed) == actor) {
        return hint;
    }
    for (int i = 0; i < count; i++) {
        if (atomic_load_explicit(&table->slots[i], memory_order_relaxed) == actor) return i;
    }
    return -1;
}

// Appends `actor`. The caller has checked there is room.
static inline void actor_table_push_locked(Scheduler* sched, ActorBase* actor) {
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);
    atomic_store_explicit(&table->slots[count], actor, memory_order_release);
    atomic_store_explicit(&sched->actor_count, count + 1, memory_order_release);
}

// Removes slot `i` by moving the last actor into it.
static inline void actor_table_remove_locked(Scheduler* sched, int i) {
    AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    int last = atomic_load_explicit(&sched->actor_count, memory_order_relaxed) - 1;
    ActorBase* moved = atomic_load_explicit(&table->slots[last], memory_order_relaxed);
    atomic_store_explicit(&table->slots[i], moved, memory_order_release);
    atomic_store_explicit(&table->slots[last], NULL, memory_order_release);
    atomic_store_explicit(&sched->actor_count, last, memory_order_release);
}

// Replaces a full table with one twice its size. The new table is published
// before the caller stores the count that needs it, and the old one is kept
// for readers still walking it. Returns 0, or -1 when the allocation fails.
static int actor_table_grow_locked(Scheduler* sched, int numa_node) {
    AetherActorTable* old = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
    int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);
    AetherActorTable* table = actor_table_alloc(old->capacity * 2, numa_node);
    if (!table) return -1;
    for (int i = 0; i < count; i++) {
        atomic_store_explicit(&table->slots[i],
                              atomic_load_explicit(&old->slots[i], memory_order_relaxed),
                              memory_order_relaxed);
    }
    table->retired = old;
    atomic_store_explicit(&sched->actor_table, table, memory_order_release);
    return 0;
}

// ---------------------------------------------------------------------------
// Actor reclamation (#2509)
//
// The table readers above take no lock, so taking a released actor out of its
// table is not enough to free it: a reader may have loaded its pointer before
// the removal, or still walk a table that was grown out of and keeps a stale
// copy of it. A released actor is therefore retired and freed later, once no
// reader can still hold it. The readers are of two kinds.
//
//  - Each core's scheduler thread is always reading: it walks its table and
//    steps actors for the whole of a loop iteration. The top of the loop is
//    its quiescent point, where it holds no actor pointer from a table. There
//    it publishes the global epoch it has just loaded (acquire) in its
//    `reclaim_epoch`. The thread starts reading only after publishing an
//    epoch (with a seq_cst fence, below) and stores 0 when it exits.
//
//  - Any other thread that walks a table (aether_scheduler_poll(),
//    scheduler_wait()) brackets the walk: it claims one of the external
//    slots with the global epoch, issues a seq_cst fence, walks, and stores 0.
//
// Releasing removes the actor from its table under the core's lock, issues a
// seq_cst fence, and takes E = the global epoch while incrementing it. The
// actor is freed once every published epoch is 0 or greater than E. Why that
// is safe, for a reader R that could have found the actor:
//
//  - R's slot store and fence, against the releaser's removal and fence, is
//    the store-buffering pattern: one of the two fences comes first in the
//    single order of seq_cst fences. If the releaser's comes first, R's table
//    loads after its own fence see the removal (and the newest table, whose
//    grow happened before the removal under the lock), so R never finds the
//    actor. If R's comes first, the reclaimer, which reads the slots after the
//    releaser's fence (through the retired list's lock), sees R's slot store
//    or a later one, and the epoch R stored was loaded before R's fence, so it
//    is at most E. Either R does not hold the actor, or its slot keeps it.
//
//  - A core thread's per-iteration store has no fence. If the reclaimer reads
//    a value greater than E there, the thread loaded that value from the
//    global epoch, which was written by the increment that produced E + 1 or a
//    later one; its acquire load synchronizes with that release, so
//    everything the thread reads afterwards sees the removal. A stale (older)
//    value only delays the free.
//
// Hence no reader dereferences a freed actor, and no reader takes a lock. A
// core blocked in a long step holds the epoch back and delays every free
// until the step returns; an idle core still wakes every PARK_MS_MAX at most.
// ---------------------------------------------------------------------------

#define AETHER_EXTERNAL_READER_SLOTS 32

static _Atomic uint64_t g_reclaim_epoch = 1;  // never 0: 0 means "not reading"
static _Atomic uint64_t g_external_reader[AETHER_EXTERNAL_READER_SLOTS];

// Set on a core's scheduler thread, whose own `reclaim_epoch` already covers
// any walk it makes (aether_scheduler_poll() called from a step, say).
static AETHER_TLS int t_reclaim_core_thread = 0;

// Readers that are neither a core thread nor a bracketed walk (#2517): a
// per-actor thread (aether_actor_thread) runs a step whose sends read their
// targets' state (dead, assigned_core, inflight) and may touch them after the
// step. It sends as a foreign thread (#3246), so it never steps another actor
// itself, but it reads them as a core does. Such a thread holds a
// record for as long as it runs and treats it as a core treats its
// `reclaim_epoch`: the epoch in it is refreshed at the top of its loop, the
// one point where it holds no actor it found through a table. The records
// live for the process (a thread exits without a join to wait for, so none
// is freed) and are reused; the list only grows.
typedef struct ReclaimReader {
    _Atomic uint64_t epoch;      // 0 while nothing reads through this record
    _Atomic int in_use;
    struct ReclaimReader* next;
} ReclaimReader;

static _Atomic(ReclaimReader*) g_readers = NULL;
static AETHER_TLS ReclaimReader* t_reader = NULL;

void scheduler_reader_online(void) {
    if (t_reader) return;
    ReclaimReader* r = atomic_load_explicit(&g_readers, memory_order_acquire);
    for (; r; r = r->next) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&r->in_use, &expected, 1,
                                                    memory_order_acquire,
                                                    memory_order_relaxed)) {
            break;
        }
    }
    if (!r) {
        r = (ReclaimReader*)calloc(1, sizeof(ReclaimReader));
        if (!r) {
            // Without a record the thread cannot hold a free back: it
            // must not step at all, which the caller cannot do either.
            // Reading on is the use after free of #2517; say so.
            fprintf(stderr, "aether: out of memory registering a reader thread\n");
            return;
        }
        atomic_store_explicit(&r->in_use, 1, memory_order_relaxed);
        ReclaimReader* head = atomic_load_explicit(&g_readers, memory_order_relaxed);
        do {
            r->next = head;
        } while (!atomic_compare_exchange_weak_explicit(&g_readers, &head, r,
                                                        memory_order_release,
                                                        memory_order_relaxed));
    }
    t_reader = r;
    // As reclaim_core_online: publish first, then the fence that pairs with
    // a releaser's.
    uint64_t epoch = atomic_load_explicit(&g_reclaim_epoch, memory_order_acquire);
    atomic_store_explicit(&r->epoch, epoch, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
}

void scheduler_reader_quiescent(void) {
    ReclaimReader* r = t_reader;
    if (!r) return;
    // The same store a core makes at the top of its loop.
    uint64_t epoch = atomic_load_explicit(&g_reclaim_epoch, memory_order_acquire);
    if (epoch != atomic_load_explicit(&r->epoch, memory_order_relaxed)) {
        atomic_store_explicit(&r->epoch, epoch, memory_order_release);
    }
}

void scheduler_reader_offline(void) {
    ReclaimReader* r = t_reader;
    if (!r) return;
    t_reader = NULL;
    atomic_store_explicit(&r->epoch, 0, memory_order_release);
    atomic_store_explicit(&r->in_use, 0, memory_order_release);
}

typedef struct {
    ActorBase* actor;
    uint64_t epoch;
} RetiredActor;

// Released actors in the order of their epochs, oldest first. Guarded by
// g_retired_lock, which only releasing and reclaiming take.
static atomic_flag g_retired_lock = ATOMIC_FLAG_INIT;
static RetiredActor* g_retired = NULL;
static int g_retired_count = 0;
static int g_retired_capacity = 0;
static _Atomic int g_retired_pending = 0;  // g_retired_count, read without the lock

static inline void retired_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_retired_lock, memory_order_acquire)) {
        AETHER_PAUSE();
    }
}

static inline int retired_trylock(void) {
    return !atomic_flag_test_and_set_explicit(&g_retired_lock, memory_order_acquire);
}

static inline void retired_unlock(void) {
    atomic_flag_clear_explicit(&g_retired_lock, memory_order_release);
}

// A core thread starts reading: publish an epoch, then the fence that pairs
// with a releaser's (the first bullet above).
static void reclaim_core_online(Scheduler* sched) {
    t_reclaim_core_thread = 1;
    uint64_t epoch = atomic_load_explicit(&g_reclaim_epoch, memory_order_acquire);
    atomic_store_explicit(&sched->reclaim_epoch, epoch, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
}

static void reclaim_core_offline(Scheduler* sched) {
    atomic_store_explicit(&sched->reclaim_epoch, 0, memory_order_release);
    t_reclaim_core_thread = 0;
}

// Claims an external reader slot; returns its index, or -1 when the caller is
// a core thread or a registered reader thread, whose own slot or record
// already covers it.
static int reclaim_reader_enter(void) {
    if (t_reclaim_core_thread || t_reader) return -1;
    for (;;) {
        uint64_t epoch = atomic_load_explicit(&g_reclaim_epoch, memory_order_acquire);
        for (int i = 0; i < AETHER_EXTERNAL_READER_SLOTS; i++) {
            uint64_t expected = 0;
            if (atomic_load_explicit(&g_external_reader[i], memory_order_relaxed) == 0 &&
                atomic_compare_exchange_strong_explicit(&g_external_reader[i], &expected, epoch,
                                                        memory_order_relaxed,
                                                        memory_order_relaxed)) {
                atomic_thread_fence(memory_order_seq_cst);
                return i;
            }
        }
        // More concurrent walkers than slots: wait for one to finish.
        aether_sched_yield();
    }
}

static void reclaim_reader_exit(int slot) {
    if (slot >= 0) atomic_store_explicit(&g_external_reader[slot], 0, memory_order_release);
}

// ---------------------------------------------------------------------------
// Released actors keep their memory (#2517)
//
// A send takes a raw ActorBase* and reads `dead` first. Nothing may send to a
// released actor, but a send that breaks the contract must not read freed
// memory, so a reclaimed actor's block does not go back to the allocator: it
// stays marked released, in the bucket for its size, and the next spawn of
// that size takes it back. A late send to it is dropped, counted and reported
// once. One made after the block has become another actor reaches that
// actor: a raw pointer cannot tell the two apart. A block goes back to its
// bucket once it is reclaimed, and a spawn takes it once no message is on
// its way to it; a spawn before then takes a fresh block. So a bucket holds
// at most as many blocks as were once, at the same time, live, released and
// not yet reclaimed, or awaited by a message: how many depends on the cores'
// timing as well as on the program (#3385). The buckets are freed with the
// tables.
// ---------------------------------------------------------------------------

typedef struct {
    size_t size;
    ActorBase** blocks;
    int count;
    int capacity;
} TombstoneBucket;

static atomic_flag g_tombstone_lock = ATOMIC_FLAG_INIT;
static TombstoneBucket* g_tombstones = NULL;
static int g_tombstone_buckets = 0;
static int g_tombstone_bucket_cap = 0;

static inline void tombstone_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_tombstone_lock, memory_order_acquire)) {
        AETHER_PAUSE();
    }
}

static inline void tombstone_unlock(void) {
    atomic_flag_clear_explicit(&g_tombstone_lock, memory_order_release);
}

static TombstoneBucket* tombstone_bucket_locked(size_t size, int create) {
    for (int i = 0; i < g_tombstone_buckets; i++) {
        if (g_tombstones[i].size == size) return &g_tombstones[i];
    }
    if (!create) return NULL;
    if (g_tombstone_buckets == g_tombstone_bucket_cap) {
        int cap = g_tombstone_bucket_cap ? g_tombstone_bucket_cap * 2 : 8;
        TombstoneBucket* grown = realloc(g_tombstones, (size_t)cap * sizeof(TombstoneBucket));
        if (!grown) return NULL;
        g_tombstones = grown;
        g_tombstone_bucket_cap = cap;
    }
    TombstoneBucket* b = &g_tombstones[g_tombstone_buckets++];
    b->size = size;
    b->blocks = NULL;
    b->count = 0;
    b->capacity = 0;
    return b;
}

// Keeps a reclaimed actor's block for the next spawn of its size.
static void tombstone_keep(ActorBase* actor) {
    tombstone_lock();
    TombstoneBucket* b = tombstone_bucket_locked(actor->alloc_size, 1);
    if (b && b->count == b->capacity) {
        int cap = b->capacity ? b->capacity * 2 : 16;
        ActorBase** grown = realloc(b->blocks, (size_t)cap * sizeof(ActorBase*));
        if (grown) {
            b->blocks = grown;
            b->capacity = cap;
        }
    }
    if (b && b->count < b->capacity) {
        b->blocks[b->count++] = actor;
        tombstone_unlock();
        return;
    }
    tombstone_unlock();
    // Out of memory to keep it: a late send would then read freed memory,
    // which is the contract violation's own risk; the block goes back.
    aether_numa_free_aligned(actor, actor->alloc_size);
}

// The most recently kept block of this size that no message is on its way
// to, or NULL. A message sent to the released actor before its release can
// still be in a core's queue, counted in the block's `inflight` (#3090); the
// core that dequeues it drops it, the block still marked released, and
// uncounts it. Handed to a new actor first, the block would have taken that
// message as the new actor's, and its uncount from the new actor's count.
static ActorBase* tombstone_take(size_t size) {
    ActorBase* actor = NULL;
    tombstone_lock();
    TombstoneBucket* b = tombstone_bucket_locked(size, 0);
    for (int k = b ? b->count - 1 : -1; k >= 0 && !actor; k--) {
        if (atomic_load_explicit(&b->blocks[k]->inflight, memory_order_acquire) != 0) continue;
        // Nor one a core's ready list or timer heap still names (#3540): the
        // reclaim keeps those, so none is kept here, but a block is handed
        // to a new actor only when nothing of the scheduler's refers to it.
        if (atomic_load_explicit(&b->blocks[k]->ready_queued, memory_order_acquire) != 0 ||
            atomic_load_explicit(&b->blocks[k]->timer_slot, memory_order_acquire) != 0) continue;
        actor = b->blocks[k];
        b->blocks[k] = b->blocks[--b->count];
    }
    tombstone_unlock();
    return actor;
}

// Returns every kept block to the allocator: the scheduler's tables are
// going, and with them any pointer a late send could still use.
static void tombstone_free_all(void) {
    tombstone_lock();
    for (int i = 0; i < g_tombstone_buckets; i++) {
        TombstoneBucket* b = &g_tombstones[i];
        for (int k = 0; k < b->count; k++) {
            aether_numa_free_aligned(b->blocks[k], b->blocks[k]->alloc_size);
        }
        free(b->blocks);
    }
    free(g_tombstones);
    g_tombstones = NULL;
    g_tombstone_buckets = 0;
    g_tombstone_bucket_cap = 0;
    tombstone_unlock();
}

// Sends dropped because their target had been released, and the one
// diagnostic they get.
static _Atomic uint64_t g_released_sends = 0;
static atomic_flag g_released_send_reported = ATOMIC_FLAG_INIT;

uint64_t scheduler_released_sends(void) {
    return atomic_load_explicit(&g_released_sends, memory_order_relaxed);
}

// A message `actor` will not handle: what travels with it is released,
// what its fields own among it (#3142), and an ask's asker learns that no
// reply is coming. Any processed credit is the caller's: a message to a dead
// actor is not counted yet, one taken from its queues is. A released target
// means a send against the release contract.
static void release_undelivered(ActorBase* actor, Message* msg) {
    aether_mailbox_untake(actor, msg->type);   // its place under a bound (#2974)
    aether_message_drop(actor, msg);
    if (msg->_reply_slot) reply_slot_fail((ActorReplySlot*)msg->_reply_slot, AETHER_ASK_DEAD);
}

/* As release_undelivered, for what an ended actor leaves undelivered as its
 * death is finalized (#3401): a panic in the release is reported and the
 * drain goes on (aether_message_drop_at_end). */
static void release_undelivered_at_end(ActorBase* actor, Message* msg) {
    aether_mailbox_untake(actor, msg->type);
    aether_message_drop_at_end(actor, msg);
    if (msg->_reply_slot) reply_slot_fail((ActorReplySlot*)msg->_reply_slot, AETHER_ASK_DEAD);
}

static void send_to_dead_actor(ActorBase* actor, Message* msg) {
    AETHER_TRACE_EVENT(AE_TRACE_DROP_DEAD, actor ? actor->id : 0, msg->type, msg->sender_id);
    release_undelivered(actor, msg);
    if (actor && (atomic_load_explicit(&actor->dead, memory_order_relaxed) & AETHER_ACTOR_RELEASED)) {
        atomic_fetch_add_explicit(&g_released_sends, 1, memory_order_relaxed);
        if (!atomic_flag_test_and_set_explicit(&g_released_send_reported, memory_order_relaxed)) {
            fprintf(stderr, "aether: message type %d sent to actor %d after its release; "
                            "dropped (reported once)\n", msg->type, actor->id);
        }
    } else if (actor) {
        scheduler_count_dead_sends(1);   // a panicked, killed or stopped one (#2974)
    }
}

// Plain sends dropped at a full bounded mailbox (#2974), and their one
// diagnostic.
static _Atomic uint64_t g_full_drops = 0;
static atomic_flag g_full_drop_reported = ATOMIC_FLAG_INIT;

uint64_t scheduler_full_drops(void) {
    return atomic_load_explicit(&g_full_drops, memory_order_relaxed);
}

/* A send to a bounded actor (#2974): 1 when `msg` is accepted (or the
 * actor has no bound). Refused, it is released here, never blocked on:
 * an ask learns at once that the mailbox is full, a plain send is counted
 * in the actor's full_drops and the total, reported once. It never took a
 * place, so the release is not release_undelivered's. */
static int bound_admits(ActorBase* actor, Message* msg) {
    if (aether_mailbox_admit(actor, msg->type)) return 1;
    aether_message_drop(actor, msg);
    if (msg->_reply_slot) {
        reply_slot_fail((ActorReplySlot*)msg->_reply_slot, AETHER_ASK_FULL);
        return 0;
    }
    atomic_fetch_add_explicit(&actor->full_drops, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_full_drops, 1, memory_order_relaxed);
    if (!atomic_flag_test_and_set_explicit(&g_full_drop_reported, memory_order_relaxed)) {
        fprintf(stderr, "aether: message type %d to actor %d dropped: its mailbox is full "
                        "(bound %d); counted by actors.full_drops (reported once)\n",
                msg->type, actor->id,
                atomic_load_explicit(&actor->mailbox_bound, memory_order_relaxed));
    }
    return 0;
}

/* #2912: a thread the scheduler does not own (the main thread, a std.worker
 * job, an http pool worker, a C host's thread) reaches a core's queues and
 * lock only while the scheduler runs. It counts itself in g_foreign_senders,
 * then reads the state; teardown changes the state, then waits for the
 * count to drain before it touches a core. So a sender either sees the
 * scheduler not running, and its send is dropped and reported, or finishes
 * before any core goes. */
static _Atomic uint64_t g_stopped_sends = 0;
static atomic_flag g_stopped_send_reported = ATOMIC_FLAG_INIT;

uint64_t scheduler_stopped_sends(void) {
    return atomic_load_explicit(&g_stopped_sends, memory_order_relaxed);
}

static int foreign_send_enter(void) {
    atomic_fetch_add(&g_foreign_senders, 1);
    if (atomic_load(&g_sched_state) == 2) return 1;
    atomic_fetch_sub(&g_foreign_senders, 1);
    return 0;
}

static void foreign_send_leave(void) {
    atomic_fetch_sub(&g_foreign_senders, 1);
}

static void send_while_stopped(ActorBase* actor, Message* msg) {
    release_undelivered(actor, msg);
    atomic_fetch_add_explicit(&g_stopped_sends, 1, memory_order_relaxed);
    if (!atomic_flag_test_and_set_explicit(&g_stopped_send_reported, memory_order_relaxed)) {
        fprintf(stderr, "aether: message type %d sent to actor %d while the scheduler is not "
                        "running; dropped (reported once)\n", msg->type, actor ? actor->id : 0);
    }
}

/* Teardown's side, once g_sched_state no longer says running. */
static void wait_for_foreign_senders(void) {
    while (atomic_load(&g_foreign_senders) != 0) AETHER_PAUSE();
}

AetherActorRef scheduler_actor_ref(ActorBase* actor) {
    AetherActorRef ref = { actor, actor ? __atomic_load_n(&actor->id, __ATOMIC_ACQUIRE) : 0 };
    return ref;
}

void scheduler_send_ref(AetherActorRef ref, Message msg) {
    if (ref.actor && __atomic_load_n(&ref.actor->id, __ATOMIC_ACQUIRE) == ref.id) {
        scheduler_send_remote(ref.actor, msg, current_core_id);
        return;
    }
    // The block is another actor's now: the one the reference was taken
    // from was released. Its message definitions need not be the
    // sender's, so the payload alone is freed.
    release_undelivered(NULL, &msg);
    atomic_fetch_add_explicit(&g_released_sends, 1, memory_order_relaxed);
    if (!atomic_flag_test_and_set_explicit(&g_released_send_reported, memory_order_relaxed)) {
        fprintf(stderr, "aether: message type %d sent to actor %d after its release; "
                        "dropped (reported once)\n", msg.type, ref.id);
    }
}

static void actor_free_now(ActorBase* actor) {
    // Whatever is still in the mailbox was sent against the release contract.
    // Release it and credit it as processed, as a panicked actor's drain does,
    // or scheduler_wait() would wait for it forever.
    int drained = 0;
    Message discard;
    while (mailbox_receive(&actor->mailbox, &discard)) {
        release_undelivered(actor, &discard);
        drained++;
    }
    if (drained > 0) {
        count_processed((uint64_t)drained);
    }
    // So is whatever still waits in an actor thread's inbox (#2598): it was
    // counted as sent and is released and credited the same way. The inbox
    // is the actor's alone, as is the SPSC queue send_buffer_flush makes;
    // the block kept below covers neither.
    ActorInbox* ib = atomic_exchange_explicit(&actor->inbox, NULL, memory_order_acq_rel);
    if (ib) {
        int left = 0;
        Message spill[AETHER_INBOX_RING];
        int got;
        while ((got = actor_inbox_take(ib, spill, AETHER_INBOX_RING)) > 0) {
            for (int k = 0; k < got; k++) release_undelivered(actor, &spill[k]);
            left += got;
        }
        if (left > 0) {
            count_processed((uint64_t)left);
        }
        free(ib);
    }
    // Its death notices go with it (#2974).
    aether_watch_free(actor);
    if (actor->spsc_queue) {
        free(actor->spsc_queue);
        actor->spsc_queue = NULL;
    }
    // A caller's actor (scheduler_register_actor) is the caller's to free.
    if (!actor->scheduler_owned) return;
    // What the state owns goes before the block is kept (#2528). Nothing
    // steps the actor any more: a retired one is freed only past every
    // reader's epoch, a teardown's once the threads are joined.
    if (actor->destroy_state) {
        actor->destroy_state(actor);
        actor->destroy_state = NULL;
    }
    tombstone_keep(actor);
}

// Frees every retired actor that no reader can still hold. `wait` takes the
// lock even when contended; a core thread passes 0 and tries again next time.
static void actor_reclaim(int wait) {
    if (atomic_load_explicit(&g_retired_pending, memory_order_relaxed) == 0) return;
    if (wait) retired_lock();
    else if (!retired_trylock()) return;

    // The oldest epoch any reader still holds.
    uint64_t oldest = UINT64_MAX;
    for (int c = 0; c < MAX_CORES; c++) {
        uint64_t e = atomic_load_explicit(&schedulers[c].reclaim_epoch, memory_order_acquire);
        if (e != 0 && e < oldest) oldest = e;
    }
    for (int i = 0; i < AETHER_EXTERNAL_READER_SLOTS; i++) {
        uint64_t e = atomic_load_explicit(&g_external_reader[i], memory_order_acquire);
        if (e != 0 && e < oldest) oldest = e;
    }
    for (ReclaimReader* r = atomic_load_explicit(&g_readers, memory_order_acquire);
         r; r = r->next) {
        uint64_t e = atomic_load_explicit(&r->epoch, memory_order_acquire);
        if (e != 0 && e < oldest) oldest = e;
    }

    // Past every reader's epoch, and named by no core's ready list or timer
    // heap (#3540): a core that takes such an entry reads the death bits
    // first and lets go of the actor with its last write, the clear the
    // loads below read. One still in a heap has its core told, which takes
    // it out; it waits here, in its place, until then.
    int passed = 0, kept = 0;
    while (passed < g_retired_count && g_retired[passed].epoch < oldest) {
        ActorBase* actor = g_retired[passed].actor;
        int queued = atomic_load_explicit(&actor->ready_queued, memory_order_acquire) != 0;
        int timed = atomic_load_explicit(&actor->timer_slot, memory_order_acquire) != 0;
        if (queued || timed) {
            if (!queued) ready_push(actor);
            g_retired[kept++] = g_retired[passed];
        } else {
            actor_free_now(actor);
        }
        passed++;
    }
    if (passed > kept) {
        memmove(g_retired + kept, g_retired + passed,
                (size_t)(g_retired_count - passed) * sizeof(RetiredActor));
        g_retired_count -= passed - kept;
        atomic_store_explicit(&g_retired_pending, g_retired_count, memory_order_relaxed);
    }
    retired_unlock();
}

// Waits until every released actor has been ended (its state destroyed, its
// block kept for reuse), which otherwise happens on the core threads' own
// schedule once every reader has moved past it (#2528). For a host or a test
// that wants the memory of released actors settled before it measures or
// goes on; bounded, so a core blocked in a long step cannot hang it.
int scheduler_reclaim_released(void) {
    // A parked core keeps the epoch it last published until it wakes (up to
    // PARK_MS_MAX), so the cores are woken to republish, and the wait is by
    // time, well past one park, not by spins.
    for (int rounds = 0; rounds < 2500; rounds++) {
        if (atomic_load_explicit(&g_retired_pending, memory_order_acquire) == 0) return 0;
        actor_reclaim(1);
        if (atomic_load_explicit(&g_retired_pending, memory_order_acquire) == 0) return 0;
        for (int c = 0; c < num_cores; c++) sched_wake(&schedulers[c]);
        aether_usleep(200);
    }
    return atomic_load_explicit(&g_retired_pending, memory_order_acquire);
}

// Frees every retired actor without asking. Only once no reader is left:
// the threads are joined and nothing else walks the tables.
static void actor_reclaim_all(void) {
    retired_lock();
    for (int i = 0; i < g_retired_count; i++) actor_free_now(g_retired[i].actor);
    free(g_retired);
    g_retired = NULL;
    g_retired_count = 0;
    g_retired_capacity = 0;
    atomic_store_explicit(&g_retired_pending, 0, memory_order_relaxed);
    retired_unlock();
}

// Queues an actor that is no longer in any table to be freed.
static void actor_retire(ActorBase* actor) {
    // The releaser's fence of the store-buffering pair: it orders the removal
    // the caller made before the epoch is taken.
    atomic_thread_fence(memory_order_seq_cst);
    retired_lock();
    if (g_retired_count == g_retired_capacity) {
        int cap = g_retired_capacity ? g_retired_capacity * 2 : 64;
        RetiredActor* grown = realloc(g_retired, (size_t)cap * sizeof(RetiredActor));
        if (!grown) {
            // Freeing now could hand a reader freed memory; keeping the actor
            // costs only its memory.
            retired_unlock();
            fprintf(stderr, "aether: out of memory retiring actor %d; it is not freed\n",
                    actor->id);
            return;
        }
        g_retired = grown;
        g_retired_capacity = cap;
    }
    // Taken under the lock, so the list stays in epoch order.
    uint64_t epoch = atomic_fetch_add_explicit(&g_reclaim_epoch, 1, memory_order_seq_cst);
    g_retired[g_retired_count].actor = actor;
    g_retired[g_retired_count].epoch = epoch;
    g_retired_count++;
    atomic_store_explicit(&g_retired_pending, g_retired_count, memory_order_relaxed);
    retired_unlock();
}

int scheduler_released_actors_pending(void) {
    return atomic_load_explicit(&g_retired_pending, memory_order_relaxed);
}

// Whether any from_queue of this core holds a message: the park's last look
// before it sleeps. The loops that step an actor many times yield on the
// `inbound` hint instead (inbound_pending, #3541), one load where this is
// the head and tail of every queue.
static inline int has_pending_cross_core(Scheduler* sched) {
    for (int q = 0; q <= MAX_CORES; q++) {
        if (queue_size(&sched->from_queues[q]) > 0) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The ready list and the timer heap (#3540)
//
// A core's loop looks at the actors that have something for it to do, not
// at every actor it has. It used to walk its whole table whenever no
// message came in through its queues, loading the mailbox count and
// migrate_to of each actor, idle ones included, and reading the clock for
// each actor with a receive timeout: the steps were skipped for idle
// actors, the loads were not, a cache miss per idle actor per pass. A core
// with 100,000 idle actors paid 100,000 misses on every idle pass.
//
// What an actor of a core can have for it to do, and how the core learns it:
//
//  - Messages in its mailbox. A message that comes through the core's
//    queues is delivered and stepped by the loop's batch, as before. Every
//    delivery that steps nothing pushes the actor (a send to an actor in
//    its own step or past the inline depth, a same-core remote send, a
//    flush, a relief's delivery), every loop that steps it pushes it when
//    it stops with one left (its budget spent, a yield, a step lock held
//    elsewhere), and so does a step that refills the mailbox from the
//    overflow (#3241). So a mailbox that holds a message has an entry,
//    unless a loop stepping it is under way, which pushes it as it stops.
//  - A move asked of it (#3090): a sender's ask comes with a message, which
//    the batch delivers; a work steal pushes the actor it asks for; a move
//    that fails pushes it again, so it is tried on every pass while the ask
//    stands, as the scan tried it.
//  - A receive timeout: in the core's timer heap, a min-heap of deadlines.
//    The countdown is armed at spawn (scheduler_spawn_ready tells the core,
//    through scheduler_timeout_armed), at the end of a message's handling
//    (the core reads it after the step), or by hand (scheduler_timeout_armed
//    again). Each pass reads the clock once while the heap holds anything,
//    and fires what is due. A heap key is never later than the countdown's
//    deadline: the countdown only moves later between two looks (a message
//    disarms it, its handling's end arms it again), so a key that comes due
//    is checked against the actor and moved on, or dropped when disarmed.
//    The park sleeps no longer than the earliest key.
//  - A move onto the core (move_actor pushes the actor to its new core),
//    a restart in place (scheduler_actor_revived), and an entry that reached
//    the wrong core because the actor moved after its push (that core
//    forwards it).
//
// Who pushes: the core's own thread onto its `ready` vector, any other
// thread onto its `ready_in`, a vector under a spinlock that the core takes
// into `ready` on each pass. An entry is pushed only by the push that turns
// the actor's `ready_queued` byte from 0 to 1 (an exchange), and the core
// clears it (an exchange) as it takes the entry, before it looks at it. So an
// actor has at most one entry anywhere, which a duplicate push never adds
// to, and a push that comes while the core looks either sees the byte
// cleared and pushes again, or its exchange comes first and the core's
// exchange, which reads it, sees what the pusher wrote before (the mailbox,
// the countdown). The owner core's own push skips the exchange when it reads
// the byte set: the entry is then in its own lists, or in another core's,
// which forwards it here.
//
// The entry is in the lists of the core the actor was on when it was
// pushed. A move does not take it out (the source would have to search its
// list): the source takes it later, sees the actor is another core's, and
// pushes it there. While the source waits in an ask, its relief does that
// for the actors it moved away (ready_forward_moved). The timer heap entry
// does go with a move: the source removes it (timer_slot gives its place)
// before the actor is the destination's, and the destination reads the
// countdown when it takes the actor.
//
// A released actor (#2509) keeps its block until no entry refers to it: the
// reclaim frees it only once `ready_queued` and `timer_slot` are both 0, and
// wakes the core of one still in a heap. A core that takes an entry reads
// the death bits before it clears `ready_queued`; it touches nothing of a
// released actor after that clear and the removal of its timer, so a
// reclaim that sees both 0 has nothing to wait for. A core that reads the
// actor alive is in a pass that began before the release was retired, which
// holds the actor's epoch back (the reclamation notes above).
//
// The pass is O(actors with work), not O(actors). The cost on the busy
// paths: a load of the receive timeout's interval after each step (on the
// line the step's death check reads), the mailbox count a loop already
// reads as it stops, and one exchange each way when it left a message.
// ---------------------------------------------------------------------------

/* The entries a core's ready lists and timer heap have room for when
 * scheduler_init makes them; each grows past it on demand. */
#define READY_INITIAL 64

typedef struct AetherTimer {
    uint64_t deadline;
    ActorBase* actor;
} AetherTimer;

/* Out of memory for a ready list or the timer heap: an actor with work left
 * where no core would look at it waits forever, so the process stops, as an
 * out-of-memory deferred send does (overflow_append). */
static void ready_out_of_memory(ActorBase* actor, const char* what) {
    aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
    fprintf(stderr, "aether: out of memory growing a core's %s (actor %d)\n", what,
            actor ? actor->id : 0);
    abort();
}

static void ready_vec_push(ActorBase*** v, int* n, int* cap, ActorBase* actor) {
    if (unlikely(*n == *cap)) {
        int grown_cap = *cap ? *cap * 2 : READY_INITIAL;
        ActorBase** grown = (ActorBase**)realloc(*v, (size_t)grown_cap * sizeof(ActorBase*));
        if (!grown) ready_out_of_memory(actor, "ready list");
        *v = grown;
        *cap = grown_cap;
    }
    (*v)[(*n)++] = actor;
}

/* An entry from another thread: into the core's ready_in, and the core is
 * woken (its park reads ready_in.count after its fence, sched_wake has the
 * other). */
static void ready_push_in(Scheduler* s, ActorBase* actor) {
    while (atomic_flag_test_and_set_explicit(&s->ready_in.lock, memory_order_acquire)) AETHER_PAUSE();
    int n = atomic_load_explicit(&s->ready_in.count, memory_order_relaxed);
    ready_vec_push(&s->ready_in.list, &n, &s->ready_in.cap, actor);
    atomic_store_explicit(&s->ready_in.count, n, memory_order_release);
    atomic_flag_clear_explicit(&s->ready_in.lock, memory_order_release);
    sched_wake(s);
}

/* What other threads pushed, into this core's own list. */
static void ready_take_in(Scheduler* s) {
    if (likely(atomic_load_explicit(&s->ready_in.count, memory_order_acquire) == 0)) return;
    while (atomic_flag_test_and_set_explicit(&s->ready_in.lock, memory_order_acquire)) AETHER_PAUSE();
    int n = atomic_load_explicit(&s->ready_in.count, memory_order_relaxed);
    for (int i = 0; i < n; i++)
        ready_vec_push(&s->ready, &s->ready_count, &s->ready_cap, s->ready_in.list[i]);
    atomic_store_explicit(&s->ready_in.count, 0, memory_order_relaxed);
    atomic_flag_clear_explicit(&s->ready_in.lock, memory_order_release);
}

/* The actor has work for its core: one entry for it in that core's lists,
 * unless one is there already. From any thread. */
static void ready_push(ActorBase* actor) {
    if (unlikely(actor->auto_process)) return;   // its own thread runs it
    int me = t_core;
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    // Its own core reading the byte set: the entry is in its own lists, or
    // in another core's, which forwards it here; this thread takes it after
    // what it wrote before this call.
    if (me >= 0 && core == me && atomic_load_explicit(&actor->ready_queued, memory_order_relaxed)) return;
    if (atomic_exchange_explicit(&actor->ready_queued, 1, memory_order_acq_rel)) return;
    // Read again under the claim: a move since the read above has its new
    // core take the entry (or forward it from the old one).
    core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    if (unlikely(core < 0 || core >= num_cores)) {
        // On no core (not registered, or the scheduler is gone): no core
        // would take the entry, and none looks at the actor.
        atomic_store_explicit(&actor->ready_queued, 0, memory_order_release);
        return;
    }
    Scheduler* s = &schedulers[core];
    if (core == me) ready_vec_push(&s->ready, &s->ready_count, &s->ready_cap, actor);
    else ready_push_in(s, actor);
}

/* An entry `sched` took for an actor that is core `core`'s: passed there as
 * it is, with ready_queued still set, so no push meanwhile finds the actor
 * in no list and adds a second entry, and no deregister (which waits for
 * ready_queued to clear) sees it in none while this thread still reads it:
 * nothing of the actor is read here once the entry is in the other core's
 * list. An actor on no core any more has its entry dropped. */
static void ready_pass_on(Scheduler* sched, ActorBase* actor, int core) {
    if (core == sched->core_id)
        ready_vec_push(&sched->ready, &sched->ready_count, &sched->ready_cap, actor);
    else if (core >= 0 && core < num_cores)
        ready_push_in(&schedulers[core], actor);
    else
        atomic_store_explicit(&actor->ready_queued, 0, memory_order_release);
}

/* The heap's slot i holds `t`, and the actor knows where. */
static inline void timer_place(Scheduler* s, int i, AetherTimer t) {
    s->timers[i] = t;
    atomic_store_explicit(&t.actor->timer_slot, i + 1, memory_order_relaxed);
}

static void timer_sift_up(Scheduler* s, int i) {
    AetherTimer t = s->timers[i];
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (s->timers[parent].deadline <= t.deadline) break;
        timer_place(s, i, s->timers[parent]);
        i = parent;
    }
    timer_place(s, i, t);
}

static void timer_sift_down(Scheduler* s, int i) {
    AetherTimer t = s->timers[i];
    int n = s->timer_count;
    for (;;) {
        int child = 2 * i + 1;
        if (child >= n) break;
        if (child + 1 < n && s->timers[child + 1].deadline < s->timers[child].deadline) child++;
        if (s->timers[child].deadline >= t.deadline) break;
        timer_place(s, i, s->timers[child]);
        i = child;
    }
    timer_place(s, i, t);
}

/* The heap index of `actor`, or -1: its slot, checked against the heap. */
static inline int timer_index(Scheduler* s, ActorBase* actor) {
    int i = atomic_load_explicit(&actor->timer_slot, memory_order_relaxed) - 1;
    return (i >= 0 && i < s->timer_count && s->timers[i].actor == actor) ? i : -1;
}

static void timer_set_key(Scheduler* s, int i, uint64_t deadline) {
    uint64_t was = s->timers[i].deadline;
    s->timers[i].deadline = deadline;
    if (deadline < was) timer_sift_up(s, i);
    else timer_sift_down(s, i);
}

static void timer_insert(Scheduler* s, ActorBase* actor, uint64_t deadline) {
    if (unlikely(s->timer_count == s->timer_cap)) {
        int cap = s->timer_cap ? s->timer_cap * 2 : READY_INITIAL;
        AetherTimer* grown = (AetherTimer*)realloc(s->timers, (size_t)cap * sizeof(AetherTimer));
        if (!grown) ready_out_of_memory(actor, "timer heap");
        s->timers = grown;
        s->timer_cap = cap;
    }
    AetherTimer t = { deadline, actor };
    s->timers[s->timer_count++] = t;
    timer_sift_up(s, s->timer_count - 1);
}

/* The heap's entry i goes; the actor it named is not touched. */
static void timer_remove_at(Scheduler* s, int i) {
    int last = --s->timer_count;
    if (i != last) {
        timer_place(s, i, s->timers[last]);
        if (i > 0 && s->timers[i].deadline < s->timers[(i - 1) / 2].deadline) timer_sift_up(s, i);
        else timer_sift_down(s, i);
    }
}

/* Out of the heap. Its slot is cleared last: the reclaim of a released
 * actor reads it, and so does a deregister (scheduler_deregister_actor). */
static void timer_remove(Scheduler* s, ActorBase* actor) {
    int i = timer_index(s, actor);
    if (i < 0) return;
    timer_remove_at(s, i);
    atomic_store_explicit(&actor->timer_slot, 0, memory_order_release);
}

/* The heap follows the actor's countdown, on its core's thread: in it with
 * a key no later than the deadline while the countdown is armed, out of it
 * while it is not. */
static void timer_sync(Scheduler* s, ActorBase* actor) {
    uint64_t armed = actor->last_activity_ns;
    uint64_t interval = actor->timeout_ns;
    int i = timer_index(s, actor);
    if (interval == 0 || armed == 0 || !actor->step ||
        atomic_load_explicit(&actor->dead, memory_order_acquire) != 0 ||
        (atomic_load_explicit(&actor->exit_flags, memory_order_acquire) & AETHER_EXIT_DEREGISTERED)) {
        if (i >= 0) timer_remove(s, actor);
        return;
    }
    uint64_t deadline = armed + interval;
    if (i < 0) timer_insert(s, actor, deadline);
    else if (deadline < s->timers[i].deadline) timer_set_key(s, i, deadline);
}

/* Takes `actor` out of `s`'s ready lists (each entry naming it becomes NULL,
 * which a pass skips) and its countdown out of `s`'s timer heap. On `s`'s
 * own thread, or while no thread runs `s`'s loop. Returns whether an entry
 * was found: ready_queued is then the caller's to clear. */
static int ready_forget_on(Scheduler* s, ActorBase* actor) {
    timer_remove(s, actor);
    int found = 0;
    for (int i = 0; i < s->ready_count; i++)
        if (s->ready[i] == actor) { s->ready[i] = NULL; found = 1; }
    for (int i = s->ready_at; i < s->ready_work_count; i++)
        if (s->ready_work[i] == actor) { s->ready_work[i] = NULL; found = 1; }
    while (atomic_flag_test_and_set_explicit(&s->ready_in.lock, memory_order_acquire)) AETHER_PAUSE();
    int n = atomic_load_explicit(&s->ready_in.count, memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (s->ready_in.list[i] == actor) { s->ready_in.list[i] = NULL; found = 1; }
    atomic_flag_clear_explicit(&s->ready_in.lock, memory_order_release);
    return found;
}

/* After a step of a core's actor with a receive timeout, on the thread that
 * stepped it, which is its core's (every step of a core's actor holds its
 * step lock on its core's thread): the countdown the step left goes in the
 * heap. A message the step left needs nothing here: every delivery that
 * steps nothing pushes the actor, and every loop that steps it pushes it
 * when it stops with one left. */
static void ready_after_step(ActorBase* actor) {
    int me = t_core;
    if (me >= 0 && atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == me)
        timer_sync(&schedulers[me], actor);
    else
        ready_push(actor);   // its core reads the countdown when it takes it
}

void scheduler_timeout_armed(ActorBase* actor) {
    if (actor && actor->timeout_ns != 0) ready_push(actor);
}

// Partitioned scheduler thread with work-stealing fallback for idle cores
/* A message dropped instead of delivered (its target died mid-flight)
 * still owns its payload, what its fields own, and, for asks, a
 * handler-side reply-slot reference. Release them and credit the processed
 * counter: the send was already counted, so without the credit a stream of
 * in-flight messages to a panicking actor leaves sent > processed forever
 * and parks wait_for_idle. */
static void scheduler_drop_dead_message(Scheduler* sched, ActorBase* actor, Message* msg) {
    /* Its target ended with it on its way: it is what the end left
     * undelivered, as the finalize's drain of the mailbox is, but found on
     * a core's queue after that drain, by whichever thread meets it there (a
     * core's loop, a flush, an ask's relief). The release is the program's
     * code (a field's drop) and runs contained, as the drain's does (#3401):
     * run bare on a core's thread it met no frame, and a drop that panicked
     * ended the program; met on a relief, it would have been the stepping
     * actor's panic. */
    release_undelivered_at_end(actor, msg);
    count_processed(1);
    // On its way when its target died (#2974); a released target's are its
    // own count's. Counted by the caller's finding, the target dead: a
    // restart in place may have cleared the bit since.
    if (actor && !(atomic_load_explicit(&actor->dead, memory_order_relaxed) & AETHER_ACTOR_RELEASED))
        scheduler_count_dead_sends(1);
}

// Whether nothing sent from core `core` is still on its way: no core's queue
// from it holds a message, and this thread (that core's) has no overflow.
// A core receives through one queue per sending core and drains them in
// index order, so an actor that left `core` with a message still in one of
// them could have its next message, sent from its new core, delivered
// first (#3240). Called on `core`'s own thread.
static int core_outbound_idle(int core) {
    if (tls_overflow_any) return 0;
    for (int k = 0; k < num_cores; k++)
        if (queue_size(&schedulers[k].from_queues[core]) > 0) return 0;
    return 1;
}

// Move `actor` from `sched`, its core, to core `dst`, on `sched`'s own thread
// while it steps nothing (or relieves its core from an ask, #3197): every
// move of an actor between cores is made here
// (#3090, #3093). Takes both cores' table locks without waiting, in
// ascending core order as every two-lock path does, then the actor's step
// lock, then move_begin: no message for it may be on its way through any
// core. Returns 1 when moved; the new core is woken to step whatever is in
// the mailbox that moved with it. Returns 0, the actor where it was, when a
// lock is taken or a message is in flight. `onto_waiter`: a relief's move
// (#3197), which may take the actor onto a core blocked in an ask when that
// ask waits on this very actor.
static int move_actor(Scheduler* sched, ActorBase* actor, int dst, int slot_hint, int onto_waiter) {
    if (dst < 0 || dst >= num_cores || dst == sched->core_id || actor->auto_process ||
        atomic_load_explicit(&actor->dead, memory_order_acquire))
        return 0;
    /* What the actor sent from here and is still on its way leaves no
     * order behind it: moved, its next messages leave from another core's
     * queues, which a receiver may drain first (#3240). */
    if (!core_outbound_idle(sched->core_id)) return 0;
    Scheduler* to = &schedulers[dst];
    OptimizedSpinlock* first  = sched->core_id < dst ? &sched->actor_lock : &to->actor_lock;
    OptimizedSpinlock* second = sched->core_id < dst ? &to->actor_lock : &sched->actor_lock;
    if (atomic_flag_test_and_set_explicit(&first->lock, memory_order_acquire)) return 0;
    if (atomic_flag_test_and_set_explicit(&second->lock, memory_order_acquire)) {
        atomic_flag_clear_explicit(&first->lock, memory_order_release);
        return 0;
    }
    /* Never onto a core blocked in an ask (#3220): it runs nothing until its
     * reply, and an actor taken there, the one asked among them, waited with
     * it. A handler that sent to an actor and then asked it was the common
     * case: the send asked the actor's core to gather it onto the asker's
     * (#3090), which did so as the ask's message reached it, and the round
     * waited out the timeout. Read under the destination's lock, under
     * which the asking core sets it: a move either ended before the mark,
     * and the asker finds the actor on its core and steps it, or sees it.
     * The move is asked for again once the core runs. A relief takes there
     * the actor the core waits on, and only it (#3197): the waiting thread
     * steps its own target, woken through the slot below. */
    int waiting = atomic_load_explicit(&to->asking, memory_order_acquire) > 0;
    if (waiting && !(onto_waiter &&
                     atomic_load_explicit(&to->ask_target, memory_order_acquire) == actor)) {
        atomic_flag_clear_explicit(&second->lock, memory_order_release);
        atomic_flag_clear_explicit(&first->lock, memory_order_release);
        return 0;
    }
    int moved = 0;
    int at = actor_table_has_room_locked(to) ? actor_table_find_locked(sched, actor, slot_hint) : -1;
    if (at >= 0 && !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
        if (move_begin(actor)) {
            // Its countdown leaves this core's timer heap before the actor
            // is the destination's, whose thread may put it in its own
            // heap at once; the destination reads the countdown when it
            // takes the actor from its ready list (#3540).
            timer_remove(sched, actor);
            actor_table_remove_locked(sched, at);
            atomic_store_explicit(&actor->assigned_core, dst, memory_order_release);
            atomic_store_explicit(&actor->migrate_to, -1, memory_order_relaxed);
            actor_table_push_locked(to, actor);
            move_end(actor);
            moved = 1;
        }
        atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
    }
    /* Taken to the core waiting on it: its thread waits on the ask's slot,
     * not on its park, so the slot is what wakes it. The slot stays while
     * the destination's lock is held: the asker puts ask_slot back under it
     * before it lets the slot go. */
    if (moved && waiting) {
        ActorReplySlot* slot = atomic_load_explicit(&to->ask_slot, memory_order_acquire);
        if (slot) {
            pthread_mutex_lock(&slot->mutex);
            pthread_cond_signal(&slot->cond);
            pthread_mutex_unlock(&slot->mutex);
        }
    }
    atomic_flag_clear_explicit(&second->lock, memory_order_release);
    atomic_flag_clear_explicit(&first->lock, memory_order_release);
    // The new core looks at what moved with it (its mailbox, its countdown)
    // through its ready list (#3540), which wakes it. An entry still in this
    // core's lists is forwarded there when this core takes it.
    if (moved) ready_push(actor);
    return moved;
}

// Step `actor`, this core's, once to make room in its full mailbox, so a
// message for it keeps its place instead of going to the back of a queue
// (#3093). Waits out a step lock another thread holds for a moment. Returns
// 0 when the actor will take no more messages: dead, or with no handler.
static int make_room(Scheduler* sched, ActorBase* actor) {
    if (atomic_load_explicit(&actor->dead, memory_order_acquire) || !actor->step) return 0;
    if (!atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
        // Down to half full, as the loop's drain before delivering did, so
        // a run of messages does not take the lock once per message.
        int stepped = 0;
        while (atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > MAILBOX_SIZE / 2 &&
               !atomic_load_explicit(&actor->dead, memory_order_acquire)) {
            stepped += aether_step_safe(actor);
        }
        count_processed((uint64_t)stepped);
        atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
    } else {
        AETHER_PAUSE();
    }
    // Dead by a step here (a panic, a stop): it takes no more. Its death
    // drained the mailbox, and what the caller would put there next would
    // wait, never handled nor credited, in a dead actor's mailbox.
    return !atomic_load_explicit(&actor->dead, memory_order_acquire);
}

/* An entry taken from this core's ready list (#3540), on its thread while it
 * steps nothing: the actor's messages stepped, a move asked of it made, its
 * countdown in the heap, as the scan did for each actor with work. Returns 1
 * when there was something to do. */
static int ready_service(Scheduler* sched, ActorBase* actor) {
    const int me = sched->core_id;
    // The death bits first: a released actor's block can be reclaimed once
    // `ready_queued` is 0, so nothing of it is touched after the clear.
    int dead = atomic_load_explicit(&actor->dead, memory_order_acquire);
    if (unlikely(dead & AETHER_ACTOR_RELEASED)) {
        if (atomic_load_explicit(&actor->assigned_core, memory_order_acquire) == me)
            timer_remove(sched, actor);
        atomic_store_explicit(&actor->ready_queued, 0, memory_order_release);
        return 0;
    }
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    if (unlikely(atomic_load_explicit(&actor->exit_flags, memory_order_acquire) &
                 AETHER_EXIT_DEREGISTERED)) {
        // Deregistered (scheduler_deregister_actor): its countdown leaves
        // the heap and its entry goes, and nothing of it is touched after
        // the clear, which the deregister waits for: its caller may free
        // it then.
        if (core == me) timer_remove(sched, actor);
        atomic_store_explicit(&actor->ready_queued, 0, memory_order_release);
        return 0;
    }
    if (core != me) {
        // Moved since it was pushed: its core looks at it.
        ready_pass_on(sched, actor, core);
        return 1;
    }
    // Cleared before the look: a push from now on adds an entry again.
    atomic_exchange_explicit(&actor->ready_queued, 0, memory_order_acq_rel);
    // Read again after the clear: a push made before it found the entry
    // here and added none, and what it was told of is seen now. A restart
    // in place tells the core while the actor is still dead, then clears
    // the bits and pushes again (scheduler_spawn_ready); the death read
    // above, before that push, took its countdown out of the heap with
    // nothing to put it back (#3540).
    dead = atomic_load_explicit(&actor->dead, memory_order_acquire);
    if (unlikely(dead != 0) || unlikely(!actor->step)) {
        // Ended (its finalize drains what waits) or never stepped: its
        // countdown will not fire. A restart in place pushes it again.
        timer_remove(sched, actor);
        return 0;
    }
    int did = 0;
    if (atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0) {
        did = 1;
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
        if (!atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
            int processed = 0, steps = 0;
            while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 &&
                   steps < 64 &&
                   atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == me) {
                int took = aether_step_safe(actor);
                processed += took;
                steps++;
                if (!took) break;   // dead: its finalize credits what waits
                // Yield to outer loop if cross-core messages arrived
                // (e.g. StopAnimation sent from main while actor sleeps).
                if (inbound_pending(sched)) break;
            }
            atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
            count_processed((uint64_t)processed);
            if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0) {
                // Left with messages (the budget spent, a yield): the next
                // pass. A dead actor's finalize drained its mailbox.
                ready_push(actor);
            } else if (steps > 0 &&
                       atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == me) {
                atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
            }
        } else {
            // Its step lock is another thread's for a moment (a move from
            // here waits for no step, a restart claim): looked at again on
            // the next pass, as the scan looked at it on every pass.
            ready_push(actor);
        }
    }
    // THEN: a move another core asked for (message-driven migration, or a
    // steal, #3090). Processing first ensures the actor makes progress even
    // under constant migration pressure. move_actor moves it only with no
    // message of it in flight; one that cannot move now is asked again on
    // the next pass while the request stands, as the scan asked it.
    int mig = atomic_load_explicit(&actor->migrate_to, memory_order_relaxed);
    if (unlikely(mig >= 0 && mig != me) &&
        atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == me) {
        did = 1;
        if (!move_actor(sched, actor, mig, -1, 0)) ready_push(actor);
    }
    // Its countdown, as the step or a spawn left it.
    if (actor->timeout_ns != 0 &&
        atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == me)
        timer_sync(sched, actor);
    return did;
}

/* The ready list's pass: every entry pushed before it, each serviced once.
 * What the services push goes in the next pass. */
static int ready_pass(Scheduler* sched) {
    ready_take_in(sched);
    if (sched->ready_count == 0) return 0;
    // The list becomes the pass's, and an empty vector (the last pass's)
    // takes the pushes made meanwhile.
    ActorBase** work = sched->ready;
    int work_cap = sched->ready_cap;
    sched->ready_work_count = sched->ready_count;
    sched->ready = sched->ready_work;
    sched->ready_cap = sched->ready_work_cap;
    sched->ready_count = 0;
    sched->ready_work = work;
    sched->ready_work_cap = work_cap;
    int did = 0;
    // ready_at is the core's, not a local: a relief during a step here
    // forwards the entries still ahead of it (ready_forward_moved).
    for (sched->ready_at = 0; sched->ready_at < sched->ready_work_count;) {
        ActorBase* actor = sched->ready_work[sched->ready_at++];
        if (actor && ready_service(sched, actor)) did = 1;
    }
    sched->ready_work_count = 0;
    sched->ready_at = 0;
    return did;
}

/* The receive timeouts that are due, fired by stepping their actors, as the
 * generated step fires one: armed, the interval past, the mailbox empty. */
static int timers_fire(Scheduler* sched) {
    if (likely(sched->timer_count == 0)) return 0;
    uint64_t now = aether_now_ns();
    const int me = sched->core_id;
    int did = 0;
    while (sched->timer_count > 0 && sched->timers[0].deadline <= now) {
        ActorBase* actor = sched->timers[0].actor;
        // The heap names actors of this core, each through the slot it
        // keeps (a move and a deregister take the countdown out first). An
        // entry that does not hold to that goes, its actor not stepped: one
        // whose actor's slot names another entry was removed through that
        // slot, which removed nothing, and the loop came back to it forever;
        // one whose actor is another core's had this thread step it beside
        // its core's.
        int top = timer_index(sched, actor);
        if (unlikely(top != 0 ||
                     atomic_load_explicit(&actor->assigned_core, memory_order_acquire) != me)) {
            timer_remove_at(sched, 0);
            continue;
        }
        uint64_t armed = actor->last_activity_ns;
        if (actor->timeout_ns == 0 || armed == 0 || !actor->step ||
            atomic_load_explicit(&actor->dead, memory_order_acquire) != 0 ||
            (atomic_load_explicit(&actor->exit_flags, memory_order_acquire) & AETHER_EXIT_DEREGISTERED)) {
            timer_remove(sched, actor);   // disarmed since, ended, or deregistered
            continue;
        }
        uint64_t due = armed + actor->timeout_ns;
        if (due > now) {   // armed again since its key was set
            timer_set_key(sched, 0, due);
            continue;
        }
        did = 1;
        if (atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0) {
            // A message came first: it disarms the countdown, and the step
            // that takes it arms it again (ready_after_step reads it).
            timer_remove(sched, actor);
            ready_push(actor);
            continue;
        }
        if (atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) break;   // next pass
        scheduler_countdown_enter();   // until its arm returns (#3443)
        /* A message that came first is what it takes: counted (#3464). */
        count_processed((uint64_t)aether_step_safe(actor));
        scheduler_countdown_leave();
        atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
        // The step left the countdown disarmed, or armed again from now;
        // a step that left it as it was (one that does not fire it) is
        // looked at again on the next pass, as the scan looked at it.
        int i = timer_index(sched, actor);
        if (i >= 0 && sched->timers[i].deadline <= now) {
            if (actor->last_activity_ns == armed && actor->timeout_ns != 0 &&
                atomic_load_explicit(&actor->dead, memory_order_acquire) == 0)
                timer_set_key(sched, i, now + 1);
            else
                timer_sync(sched, actor);
        }
    }
    return did;
}

/* The ms a core may park before its next receive timeout is due: `ms`, or
 * less; 0 when one is due now. */
static int timers_park_ms(Scheduler* sched, int ms) {
    if (likely(sched->timer_count == 0)) return ms;
    uint64_t now = aether_now_ns();
    uint64_t next = sched->timers[0].deadline;
    if (next <= now) return 0;
    uint64_t until = (next - now + 999999ULL) / 1000000ULL;
    return until < (uint64_t)ms ? (int)until : ms;
}

/* A relief moved actors off this core while its thread waits in an ask
 * (#3197): their entries still in this core's lists go to their new cores,
 * which would otherwise not look at them before the ask ends. */
/* An entry of this core's for an actor that is another core's now: taken
 * here, as ready_service takes one, and passed to that core. Steps nothing. */
static void ready_forward(Scheduler* sched, ActorBase* actor) {
    if (atomic_load_explicit(&actor->dead, memory_order_acquire) & AETHER_ACTOR_RELEASED) {
        atomic_store_explicit(&actor->ready_queued, 0, memory_order_release);   // the last touch
        return;
    }
    // To the core it is on now (back here, if it came back).
    ready_pass_on(sched, actor, atomic_load_explicit(&actor->assigned_core, memory_order_acquire));
}

static void ready_forward_moved(Scheduler* sched) {
    ready_take_in(sched);
    const int me = sched->core_id;
    for (int k = sched->ready_at; k < sched->ready_work_count; k++) {
        ActorBase* actor = sched->ready_work[k];
        if (actor && atomic_load_explicit(&actor->assigned_core, memory_order_acquire) != me) {
            sched->ready_work[k] = NULL;
            ready_forward(sched, actor);
        }
    }
    // Taken out of `ready` first: a forward pushes back into it an actor
    // that came back here meanwhile.
    ActorBase** moved = NULL;
    int nmoved = 0, cap_moved = 0, kept = 0;
    for (int k = 0; k < sched->ready_count; k++) {
        ActorBase* actor = sched->ready[k];
        if (!actor) continue;   // taken out by a deregister
        if (atomic_load_explicit(&actor->assigned_core, memory_order_acquire) != me)
            ready_vec_push(&moved, &nmoved, &cap_moved, actor);
        else
            sched->ready[kept++] = actor;
    }
    sched->ready_count = kept;
    for (int k = 0; k < nmoved; k++) ready_forward(sched, moved[k]);
    free(moved);
}

void* AETHER_HOT scheduler_thread(void* arg) {
    Scheduler* sched = (Scheduler*)arg;
    current_core_id = sched->core_id;
    t_core = sched->core_id;   // its ready list and timer heap are this thread's (#3540)

    // Initialize thread-local send buffer for this core
    send_buffer_init(sched->core_id);

    // TIER 2 AUTO-DETECT: Pin thread if OS supports it
    if (aether_has_cpu_pinning()) {
        pin_to_core(sched->core_id);
    }

    // Signal scheduler_start() that this thread is fully initialised and
    // ready to drain from_queues.  scheduler_start() spins on g_threads_ready
    // until it equals num_cores, so callers are guaranteed that every thread
    // is in the polling loop before scheduler_start() returns.
    atomic_fetch_add_explicit(&g_threads_ready, 1, memory_order_release);

    int idle_count = 0;
    int park_ms = PARK_MS_MIN;
    uint64_t idle_since = 0;   // when this idle stretch began (#3540)
    int spinning = 1;          // within AETHER_IDLE_SPIN_NS of it

    // From here on this thread reads its table: publish an epoch first (#2509).
    reclaim_core_online(sched);
    unsigned reclaim_tick = 0;

    while (atomic_load_explicit(&sched->running, memory_order_acquire)) {
        int work_done = 0;

        // Quiescent point (#2509): no actor pointer from a table is held here.
        // Publish the epoch now in effect. Each core tries to free right
        // after it moves its own epoch on, so the last core to move frees
        // what all of them have passed. While a free is held back (a core
        // in a long step, another thread walking the tables) the attempt is
        // repeated only every 1024 iterations, not on every spin.
        {
            uint64_t epoch = atomic_load_explicit(&g_reclaim_epoch, memory_order_acquire);
            if (epoch != atomic_load_explicit(&sched->reclaim_epoch, memory_order_relaxed)) {
                atomic_store_explicit(&sched->reclaim_epoch, epoch, memory_order_release);
                actor_reclaim(0);
            } else if (unlikely(++reclaim_tick % 1024 == 0)) {
                actor_reclaim(0);
            }
        }

        // Deaths to finalize here, posted from elsewhere (#2974).
        control_run(sched);

        // Flush any deferred sends from the previous iteration before draining
        // from_queues.  This ensures deferred messages land on every loop and
        // prevents them from indefinitely blocking behind a saturated queue.
        overflow_flush(sched->core_id);

        // Poll I/O events (non-blocking) — delivers MSG_IO_READY to actor mailboxes.
        // Must run every iteration so I/O events are picked up promptly,
        // not only when the scheduler is idle.
        scheduler_io_poll(sched, 0);

        // TIER 1 ALWAYS ON: Adaptive batch sizing
        int batch_size = sched->batch_state.current_batch_size;
        if (batch_size > SCHEDULER_DRAIN_BATCH) batch_size = SCHEDULER_DRAIN_BATCH;
        
        // TIER 1 ALWAYS ON: Message coalescing (batch dequeue reduces atomics from N to 1)
        // Drain all per-sender SPSC channels round-robin into the coalesce buffer.
        // Each channel has exactly one producer (SPSC), so no locks needed.
        {
            // The inbound hint is cleared before the queues are read, with
            // the fence that pairs with sched_wake's (#3541): a message this
            // drain misses had its sender read the cleared hint and set it
            // again.
            if (atomic_load_explicit(&sched->inbound, memory_order_relaxed)) {
                atomic_store_explicit(&sched->inbound, 0, memory_order_relaxed);
                atomic_thread_fence(memory_order_seq_cst);
            }
            int total = 0;
            for (int q = 0; q <= MAX_CORES && total < batch_size; q++) {
                int got = queue_dequeue_batch(
                    &sched->from_queues[q],
                    sched->coalesce_buffer.actors + total,
                    sched->coalesce_buffer.messages + total,
                    batch_size - total);
                total += got;
            }
            sched->coalesce_buffer.count = total;
            sched->coalesce_buffer.next = 0;   // the step sites below say how far
            if (total > 0) {
                // A full batch may have left more in the queues: the loops
                // below yield to them, as they yielded to what they read.
                if (total == batch_size)
                    atomic_store_explicit(&sched->inbound, 1, memory_order_relaxed);
                // This core's load, counted by the core alone (#3541).
                unsigned int load = (unsigned int)atomic_load_explicit(&sched->work_count, memory_order_relaxed);
                atomic_store_explicit(&sched->work_count, (int)(load + (unsigned int)total),
                                      memory_order_relaxed);
            }
        }

        // Process coalesced batch with minimal overhead
        for (int i = 0; i < sched->coalesce_buffer.count; i++) {
            ActorBase* actor = (ActorBase*)sched->coalesce_buffer.actors[i];
            // Taken into its mailbox by a relief during a step (#3197).
            if (unlikely(!actor)) continue;
            Message msg = sched->coalesce_buffer.messages[i];

            // Target died after this message was enqueued: drop it with
            // payload/slot cleanup and a processed credit, or the
            // sent/processed balance never converges (#1301 drain test).
            if (unlikely(atomic_load_explicit(&actor->dead, memory_order_acquire))) {
                /* The drop reads the actor (its release function, its death
                 * bits); the count is what keeps its block from a new spawn
                 * (tombstone_take), so it is given back after. And the entry
                 * goes: the move pass below reads the batch's actors again, by
                 * when the block can be another actor's. */
                int counted = !actor->auto_process;
                sched->coalesce_buffer.actors[i] = NULL;
                scheduler_drop_dead_message(sched, actor, &msg);
                if (counted) inflight_leave(actor, 1);
                work_done = 1;
                continue;
            }

            // Not this core's actor. A counted message holds its actor where
            // it was routed (move_begin waits for none), so this is a message
            // routed by a stale read or one of the runtime's own: forward it,
            // still counted, to the actor's core.
            // An actor whose assigned_core names no core of this lifecycle (a
            // caller's actor carried over a restart into fewer cores, or one
            // sent to before its registration): this core takes it and is
            // its mailbox's one writer from then on (#3673). Delivered here
            // while it stayed no core's, as it was, the core its
            // registration gave it wrote the mailbox too. The bit first, so
            // the registration that reads this core's id keeps it here.
            {
                int ac = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
                if (unlikely(ac < 0 || ac >= num_cores) && !actor->auto_process) {
                    atomic_fetch_or_explicit(&actor->exit_flags, AETHER_EXIT_ROUTED, memory_order_seq_cst);
                    atomic_compare_exchange_strong_explicit(&actor->assigned_core, &ac, sched->core_id,
                                                            memory_order_seq_cst, memory_order_acquire);
                }
            }
            if (unlikely(atomic_load_explicit(&actor->assigned_core, memory_order_acquire) != sched->core_id)) {
                int new_core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
#ifdef AETHER_DEBUG_ORDERING
                if (msg.type <= 2) {
                    fprintf(stderr, "[FWD c%d->c%d] actor=%d msg_type=%d\n",
                            sched->core_id, new_core, actor->id, msg.type);
                }
#endif
                if (new_core >= 0 && new_core < num_cores) {
                    // Bounded retry: actor may have migrated again during retry.
                    // On failure defer to overflow buffer — never spin indefinitely
                    // inside the scheduler loop (causes back-pressure deadlock).
                    int forwarded = 0;
                    for (int r = 0; r < 8; r++) {
                        if (queue_enqueue(&schedulers[new_core].from_queues[sched->core_id], actor, msg)) {
                            sched_wake(&schedulers[new_core]);
                            forwarded = 1; break;
                        }
                        int cur = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
                        if (cur >= 0 && cur < num_cores) new_core = cur;
                        AETHER_PAUSE();
                    }
                    if (!forwarded) overflow_append(new_core, actor, msg);
                } else if (actor->auto_process) {
                    // An actor with a thread of its own and no core: its
                    // thread takes every message in its inbox (#2598), its
                    // mailbox is the thread's alone.
                    scheduler_actor_thread_deliver(actor, msg);
                } else {
                    // Taken by no core above (another value since?): it waits
                    // here, counted, until it has one (#3673).
                    requeue_own(sched->core_id, actor, msg);
                }
                work_done = 1;
                continue;
            }

            // An actor with its own thread takes messages in its inbox
            // (#2598). Sends put them there directly; what comes through a
            // channel is the runtime's own (an I/O readiness event, say).
            if (unlikely(actor->auto_process)) {
                scheduler_actor_thread_deliver(actor, msg);
                work_done = 1;
                continue;
            }


            // From here the message is this core's to deliver, and so is
            // every message of the same actor that follows it in the batch:
            // a run (a fan-out's batch holds a worker's messages together).
            // They go into the mailbox in their order, a full mailbox made
            // room in by stepping the actor here, its owner, so no message
            // gives up its place: re-queued at the back of this core's
            // queue, as it was, a message could be overtaken by a later one
            // of the same sender (#3093). Behind the actor's overflow while
            // it holds anything (#3241). The run is then uncounted at once,
            // with every message of it the actor's (#3090), which keeps the
            // count off this loop's per-message path.
            int run_end = i + 1;
            while (run_end < sched->coalesce_buffer.count &&
                   sched->coalesce_buffer.actors[run_end] == (void*)actor) {
                run_end++;
            }
            int placed = i;
            while (placed < run_end) {
                if (deliver_owned(actor, sched->coalesce_buffer.messages[placed])) {
                    placed++;
                    continue;
                }
                sched->coalesce_buffer.next = placed;   // its steps' reliefs (#3197)
                if (!make_room(sched, actor)) break;
            }
            if (placed > i) inflight_leave(actor, placed - i);
            for (int k = placed; k < run_end; k++) {
                // Dead, or never stepped (no handler): it takes no more
                // messages, as a send to a dead actor is dropped.
                if (actor->step) {
                    scheduler_drop_dead_message(sched, actor, &sched->coalesce_buffer.messages[k]);
                } else {
                    release_undelivered(actor, &sched->coalesce_buffer.messages[k]);
                    count_processed(1);
                }
                inflight_leave(actor, 1);   // after the drop, which reads it
            }
            int delivered = placed - i;
            i = run_end - 1;
            sched->coalesce_buffer.next = run_end;
            work_done = 1;
            if (delivered == 0) continue;
            atomic_store_explicit(&actor->active, 1, memory_order_relaxed);

            // A sender on another core asked for this actor (#3090): it
            // moves there before it runs, so what it sends next leaves from
            // that core. A chain of sends (a ring, a pipeline) gathers on one
            // core this way, where its sends are local; moved after running,
            // as it used to be, each actor took the place its sender had just
            // left, and the chain only shifted.
            int ask = atomic_load_explicit(&actor->migrate_to, memory_order_relaxed);
            if (ask >= 0 && ask != sched->core_id && move_actor(sched, actor, ask, -1, 0)) {
                continue;
            }

            // Immediate processing: if actor is on our core and step_lock
            // is available, drain its mailbox right here. FUSED DELIVER +
            // PROCESS: no second pass over every actor of the core, so the
            // loop is O(batch_size) instead of O(actor_count).
            if (likely(actor->step) &&
                atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == sched->core_id &&
                !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
                int processed = 0, steps = 0;
                while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 &&
                       steps < 64 &&
                       atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == sched->core_id) {
                    int took = aether_step_safe(actor);
                    processed += took;
                    steps++;
                    if (!took) break;   // dead: its finalize credits what waits
                    // Yield to outer loop if cross-core messages arrived
                    // (e.g. StopAnimation sent from main while actor sleeps).
                    if (inbound_pending(sched)) break;
                }
                atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
                count_processed((uint64_t)processed);
                if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) == 0) {
                    if (steps > 0) atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
                } else {
                    // Left with messages (the drain's budget spent, a yield
                    // to the queues): the ready list brings this core back
                    // (#3540).
                    ready_push(actor);
                }
            } else if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0) {
                // Not stepped here (its step lock held elsewhere, no
                // handler): the same.
                ready_push(actor);
            }
        }
        sched->coalesce_buffer.next = sched->coalesce_buffer.count;

        // TIER 1 ALWAYS ON: Adjust adaptive batch size based on what we received
        adaptive_batch_adjust(&sched->batch_state, sched->coalesce_buffer.count);

        // Moves asked for while an actor was busy (its messages ran here
        // first, or another core's message was in flight): this batch's
        // actors are asked again now. O(coalesce_count), not O(actor_count).
        for (int mi = 0; mi < sched->coalesce_buffer.count; mi++) {
            ActorBase* mig_actor = (ActorBase*)sched->coalesce_buffer.actors[mi];
            if (unlikely(!mig_actor)) continue;
            int mig_to = atomic_load_explicit(&mig_actor->migrate_to, memory_order_relaxed);
            if (unlikely(mig_to >= 0 && mig_to != sched->core_id &&
                         atomic_load_explicit(&mig_actor->assigned_core, memory_order_relaxed) == sched->core_id)) {
                // Asked again on the next pass while the ask stands (#3540).
                if (!move_actor(sched, mig_actor, mig_to, -1, 0)) ready_push(mig_actor);
            }
        }

        // The actors with work this pass did not reach through the queues
        // (#3540): a ready list, not a walk of the table. See the notes on
        // the ready list and the timer heap, above move_actor:
        // the walk loaded every actor's mailbox count and migrate_to, and
        // read the clock for every actor with a receive timeout, whenever
        // no message had come in through the queues, so an idle core paid a
        // cache miss per idle actor per pass. The list holds the actors
        // whose mailbox a step or a delivery left with a message, those a
        // move or a steal asks something of, and those another thread
        // pushed (a move onto this core, a restart, a countdown armed at
        // spawn); the heap holds the armed receive timeouts. Both are looked
        // at on every pass, busy or not: what they hold has work.
        if (ready_pass(sched)) work_done = 1;
        if (timers_fire(sched)) work_done = 1;

        // Partitioned assignment: actors stay on their assigned core for cache locality.
        
        if (!work_done) {
            idle_count++;
            atomic_fetch_add_explicit(&sched->idle_cycles, 1, memory_order_relaxed);
            
            // WORK STEALING: After significant idle time, try to steal work.
            // Safe with atomic mailbox.count: mailbox_send uses release on count++
            // and mailbox_receive uses acquire on count read, establishing the
            // happens-before chain needed after a work-steal handoff on ARM64.
            if (idle_count > 5000 && idle_count % 1000 == 0) {
                // Find busiest core
                int busiest_core = -1;
                int max_work = 0;
                for (int i = 0; i < num_cores; i++) {
                    if (i == sched->core_id) continue;
                    int work = atomic_load_explicit(&schedulers[i].work_count, memory_order_relaxed);
                    if (work > max_work) {
                        max_work = work;
                        busiest_core = i;
                    }
                }

                // If found a busy core, try to steal an actor.
                // Lock in ascending core-id order to prevent deadlock
                // (same convention as migration).
                if (busiest_core >= 0 && max_work > 100) {
                    Scheduler* victim = &schedulers[busiest_core];
                    OptimizedSpinlock* first_lock  = (sched->core_id < busiest_core) ? &sched->actor_lock : &victim->actor_lock;
                    OptimizedSpinlock* second_lock = (sched->core_id < busiest_core) ? &victim->actor_lock : &sched->actor_lock;
                    ActorBase* asked = NULL;   // the actor asked for, if any

                    if (!atomic_flag_test_and_set_explicit(&first_lock->lock, memory_order_acquire)) {
                        if (!atomic_flag_test_and_set_explicit(&second_lock->lock, memory_order_acquire)) {
                            AetherActorTable* victim_table =
                                atomic_load_explicit(&victim->actor_table, memory_order_relaxed);
                            int victim_count =
                                atomic_load_explicit(&victim->actor_count, memory_order_relaxed);
                            if (victim_count > 4 && actor_table_has_room_locked(sched)) {
                                // Search backward for a stealable actor.  Only steal actors
                                // that have been activated (active flag set, or messages in
                                // mailbox).  A freshly spawned actor whose first messages
                                // are still in from_queues must NOT be stolen: changing its
                                // assigned_core mid-flight would route subsequent messages
                                // (e.g. Spawn) to the new core while earlier messages
                                // (e.g. Setup) are still queued on the old core, breaking
                                // the FIFO ordering that actors depend on for initialization.
                                // The victim moves it (#3090): this core asks
                                // for one, by migrate_to, and the victim's loop
                                // moves it with no message of it in flight, as
                                // every move between cores is made. Moved from
                                // here, it left the victim's queued messages
                                // for it behind, and the victim could be in a
                                // local send to it as it went.
                                for (int s = victim_count - 1; s >= 4; s--) {
                                    ActorBase* candidate = atomic_load_explicit(&victim_table->slots[s],
                                                                                memory_order_relaxed);
                                    if (!atomic_load_explicit(&candidate->active, memory_order_relaxed) &&
                                        atomic_load_explicit(&candidate->mailbox.count,
                                                             memory_order_relaxed) == 0) {
                                        continue;  // freshly spawned or fully idle, skip
                                    }
                                    // No core steps an actor that has its own
                                    // thread, so stealing one gains nothing,
                                    // and the move would change the core its
                                    // thread parks on and stops with (#2598).
                                    if (candidate->auto_process) continue;
                                    int none = -1;
                                    if (atomic_compare_exchange_strong_explicit(
                                            &candidate->migrate_to, &none, sched->core_id,
                                            memory_order_relaxed, memory_order_relaxed)) {
                                        atomic_fetch_add(&sched->steal_attempts, 1);
                                        asked = candidate;
                                    }
                                    break;
                                }
                            }
                            atomic_flag_clear_explicit(&second_lock->lock, memory_order_release);
                        }
                        atomic_flag_clear_explicit(&first_lock->lock, memory_order_release);
                    }
                    // The victim looks at the ask on its next pass, through its
                    // ready list (#3540): nothing else would bring it to an actor
                    // that has no message coming.
                    if (asked) ready_push(asked);
                }
            }

            // Keep spinning aggressively for high-throughput workloads
            // The spin before a park is a time, not a count of passes
            // (#3540): it was 10,000 passes, calibrated against a pass that
            // read every actor of the core; with the ready list a pass costs
            // the same with 10 actors or a million, so the spin ends sooner
            // in time (here 1.3 to 2 ms against 2 to 3 ms, and less where
            // PAUSE is short, as on AMD), and a pause between two messages
            // that outlasts it (an ask's round trip from another core on a
            // loaded runner) parks the core, whose next message pays the
            // park and its wake. The clock is read every 64 passes.
            if (idle_count == 1) {
                idle_since = aether_now_ns();
                spinning = 1;
            } else if (spinning && (idle_count & 63) == 0 &&
                       aether_now_ns() - idle_since >= AETHER_IDLE_SPIN_NS) {
                spinning = 0;
            }
            if (spinning) {
                // Tight spin with architecture-specific pause
                AETHER_PAUSE();
            } else if (tls_overflow_any) {
                // Messages of this core's still to deliver (#3611): its
                // overflow buffer holds what the target core's queue had no
                // room for, and only this thread flushes it, at the top of
                // each turn. Parked, it held them until the park ended, up
                // to PARK_MS_MAX each time: a target with nothing else to
                // do waited, and a dead one's restart waited on its count
                // in flight (#3606). It is not idle: it yields the CPU, so
                // a target core sharing it can make room, and flushes again.
                aether_sched_yield();
            } else {
                // A free held back by a reader (another thread walking the
                // tables, a core in a long step) is retried here, once per
                // park, so an idle core frees it within PARK_MS_MAX of the
                // reader leaving, not after the 1024 loop iterations a busy
                // core waits (#2509). It returns at once when nothing waits.
                actor_reclaim(0);

                // Extended idle. With descriptors registered the poller blocks
                // for its timeout and that is the sleep. With none registered
                // it returns immediately, which is what used to turn this
                // branch into an unbounded spin (#1517): a program with no I/O
                // pegged every core forever. Park on the condvar instead.
                // No longer than until the next receive timeout is due (#3540).
                int io_events = scheduler_io_poll(sched, timers_park_ms(sched, park_ms));
                if (io_events > 0) {
                    idle_count = 0;
                    park_ms = PARK_MS_MIN;
                } else if (sched->io_registered_count > 0) {
                    // The poll above already blocked for park_ms.
                    if (park_ms < PARK_MS_MAX) park_ms *= 2;
                } else {
                    pthread_mutex_lock(&sched->park_mutex);
                    atomic_store_explicit(&sched->parked, 1, memory_order_relaxed);
                    // Publish `parked` BEFORE the last look at the queues,
                    // with the fence that pairs with sched_wake's: a producer
                    // either sees parked==1 and signals, or its enqueue is
                    // seen here. Neither order sleeps on pending work.
                    atomic_thread_fence(memory_order_seq_cst);
                    // Its ready list too (#3540): another thread pushes into
                    // ready_in and then wakes it, as an enqueue does. And no
                    // longer than until its next receive timeout is due.
                    int wait_ms = atomic_load_explicit(&g_core_park_ms, memory_order_relaxed);
                    int sleep_ms = timers_park_ms(sched, wait_ms > 0 ? wait_ms : park_ms);
                    if (atomic_load_explicit(&sched->running, memory_order_acquire) &&
                        !has_pending_cross_core(sched) && sleep_ms > 0 && sched->ready_count == 0 &&
                        atomic_load_explicit(&sched->ready_in.count, memory_order_relaxed) == 0) {
                        struct timespec deadline;
                        aether_park_deadline(&deadline, sleep_ms);
                        pthread_cond_timedwait(&sched->park_cond,
                                               &sched->park_mutex, &deadline);
                    }
                    atomic_store_explicit(&sched->parked, 0, memory_order_release);
                    pthread_mutex_unlock(&sched->park_mutex);
                    if (park_ms < PARK_MS_MAX) park_ms *= 2;
                }
            }
        } else {
            idle_count = 0;
            park_ms = PARK_MS_MIN;
            atomic_store_explicit(&sched->idle_cycles, 0, memory_order_relaxed);
        }
        
        // Explicit check to ensure timely exit on all platforms
        if (!atomic_load_explicit(&sched->running, memory_order_acquire)) {
            break;
        }
    }

    // Its loop takes no more finalize requests (a death posted from now on
    // is finalized by the thread that ended the actor), and what was posted
    // is done here, now that the thread steps nothing (#2974).
    while (atomic_flag_test_and_set_explicit(&sched->control_lock, memory_order_acquire)) AETHER_PAUSE();
    sched->control_closed = 1;
    atomic_flag_clear_explicit(&sched->control_lock, memory_order_release);
    control_run(sched);

    // Final flush: drain any remaining overflow entries before thread exits.
    // scheduler_wait waits for count_pending_messages()==0 (which includes
    // g_overflow_total) before calling scheduler_stop, so normally overflow is
    // empty here.  But under extreme contention a late overflow_append could
    // slip in — flush it so no messages are lost.
    overflow_flush(sched->core_id);
    // The buffers are this thread's: they go back with it.
    overflow_release(sched);

    // Done reading: this core no longer holds back any free (#2509).
    reclaim_core_offline(sched);
    actor_reclaim(0);
    t_core = -1;
    // Its ready lists and timer heap are touched by no thread now: a
    // deregister takes an actor out of them itself (ready_forget).
    atomic_store_explicit(&sched->loop_left, 1, memory_order_release);

    aether_unwind_thread_cleanup();

    return NULL;
}

static void scheduler_free_core_tables(void);

void scheduler_init(int cores) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_sched_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        // Already initialized, or another thread is initializing it: wait for
        // that to finish so the caller can use the scheduler on return.
        while (atomic_load_explicit(&g_sched_state, memory_order_acquire) == 1) {
            AETHER_PAUSE();
        }
        return;
    }

    // Before this copy's first step, its sandbox joins another copy's count
    // holding levels this copy's steps may run inside (#3438). Through a
    // pointer, so the scheduler links nothing of the sandbox (#3393).
    void (*join)(void) = atomic_load_explicit(&aether_sandbox_join, memory_order_acquire);
    if (join) join();

    // A scheduler stopped by scheduler_shutdown() keeps its per-core tables
    // (an executable exits right after). Starting it again -- a host running
    // an --emit=lib program's aether_main() a second time -- allocates new
    // ones below, so release the previous lifecycle's first rather than leak
    // them. num_cores is still that lifecycle's count; 0 on a first init or
    // after scheduler_cleanup().
    scheduler_free_core_tables();

    // Opt-in SIGSEGV/SIGFPE/SIGBUS → panic handlers. No-op unless
    // AETHER_CATCH_SIGNALS=1 in the environment. Safe to call multiple times
    // — sigaction just overwrites.
    aether_panic_install_signal_handlers();

    // Reset join guard so the scheduler can be restarted (e.g. in tests).
    atomic_store_explicit(&g_threads_joined, 0, memory_order_relaxed);
    // Reset thread-started flag for the upcoming scheduler_start() call.
    atomic_store_explicit(&g_threads_started, 0, memory_order_relaxed);
    // Reset thread-ready counter for the upcoming scheduler_start() call.
    atomic_store_explicit(&g_threads_ready, 0, memory_order_relaxed);
    // Reset global overflow counter between scheduler lifecycles.
    atomic_store_explicit(&g_overflow_total, 0, memory_order_relaxed);
    // Reset main-thread send counter between scheduler lifecycles.
    // Without this, count_pending_messages() sees a stale sent > processed
    // delta from the prior run and scheduler_wait() spins forever.
    atomic_store_explicit(&main_thread_sent, 0, memory_order_relaxed);
    atomic_store_explicit(&g_foreign_processed, 0, memory_order_relaxed);

    // TIER 2: Auto-detect hardware capabilities first
    aether_detect_hardware();

    // Initialize the profile from env vars
    aether_init_from_env();

    // Tests call scheduler_init/cleanup in sequence: the count starts afresh.
    atomic_store_explicit(&g_aether_config.actor_count, 0, memory_order_relaxed);

    // Initialize NUMA topology detection
    (void)aether_numa_init();  // NUMA topology initialized for future use
    
    if (cores <= 0 || cores > MAX_CORES) {
        // Use auto-detected core count if not specified
        cores = cpu_recommend_cores();
        if (cores <= 0 || cores > MAX_CORES) cores = 4;
    }
    num_cores = cores;
    
    for (int i = 0; i < num_cores; i++) {
        schedulers[i].core_id = i;
        
        // NUMA-aware allocation: allocate scheduler data on same NUMA node as core
        int numa_node = aether_numa_node_of_cpu(i);
        // Not reading until its thread starts (#2509).
        atomic_store_explicit(&schedulers[i].reclaim_epoch, 0, memory_order_relaxed);
        // Every slot starts NULL (actor_table_alloc); the threads that read the
        // table are created after this, which publishes it to them.
        AetherActorTable* table = actor_table_alloc(MAX_ACTORS_PER_CORE, numa_node);
        if (!table) {
            fprintf(stderr, "ERROR: Failed to allocate memory for scheduler %d actors\n", i);
            exit(1);
        }
        atomic_store_explicit(&schedulers[i].actor_table, table, memory_order_relaxed);
        atomic_store_explicit(&schedulers[i].actor_count, 0, memory_order_relaxed);
        for (int q = 0; q <= MAX_CORES; q++) {
            queue_init(&schedulers[i].from_queues[q]);
        }
        atomic_store(&schedulers[i].running, 0);
        atomic_store(&schedulers[i].work_count, 0);
        atomic_store(&schedulers[i].steal_attempts, 0);
        atomic_store(&schedulers[i].idle_cycles, 0);
        spinlock_init(&schedulers[i].actor_lock);

        // Per-core message counters (no atomics needed - core-local)
        schedulers[i].messages_sent = 0;
        schedulers[i].messages_processed = 0;

        // TIER 1 ALWAYS ON: Initialize adaptive batching
        adaptive_batch_init(&schedulers[i].batch_state);

        // I/O event loop: per-core platform poller (epoll/kqueue/poll)
        if (aether_io_poller_init(&schedulers[i].io_poller) < 0) {
            fprintf(stderr, "WARNING: I/O poller init failed for core %d\n", i);
        }
        schedulers[i].io_map = calloc(AETHER_IO_MAX_FDS, sizeof(AetherIoEntry));
        // On failure leave the capacity at 0 so scheduler_io_register grows the
        // map lazily (realloc from NULL) on first use, instead of writing through
        // a NULL map that a non-zero capacity would let it index directly.
        schedulers[i].io_map_capacity = schedulers[i].io_map ? AETHER_IO_MAX_FDS : 0;
        schedulers[i].io_registered_count = 0;

        // This lifecycle's; scheduler_free_core_tables() destroys them.
        pthread_mutex_init(&schedulers[i].park_mutex, NULL);
        pthread_cond_init(&schedulers[i].park_cond, NULL);
        pthread_mutex_init(&schedulers[i].foreign_lock, NULL);
        atomic_store_explicit(&schedulers[i].parked, 0, memory_order_relaxed);
        atomic_store_explicit(&schedulers[i].asking, 0, memory_order_relaxed);
        atomic_store_explicit(&schedulers[i].ask_target, NULL, memory_order_relaxed);
        atomic_store_explicit(&schedulers[i].ask_slot, NULL, memory_order_relaxed);
        atomic_store_explicit(&schedulers[i].ask_asker, NULL, memory_order_relaxed);
        atomic_flag_clear_explicit(&schedulers[i].control_lock, memory_order_relaxed);
        atomic_store_explicit(&schedulers[i].control_pending, 0, memory_order_relaxed);
        schedulers[i].control = NULL;
        schedulers[i].control_count = 0;
        schedulers[i].control_cap = 0;
        schedulers[i].control_closed = 0;
        atomic_store_explicit(&schedulers[i].loop_left, 0, memory_order_relaxed);
        schedulers[i].relief_cursor = 0;
        schedulers[i].coalesce_buffer.count = 0;
        schedulers[i].coalesce_buffer.next = 0;
        // The ready list and the timer heap (#3540): made here with room for
        // READY_INITIAL entries each, and grown past that on demand. Made
        // on first use, as they were, a core's first push or countdown in a
        // program's steady state allocated them then: memory held for the
        // lifecycle, which a test that measures a window of steady work for
        // growth (test_actor_ref_state_store) read as a leak whenever the
        // window was the one that first stepped some core. Out of memory
        // here, they start empty and grow on first use.
        schedulers[i].ready = (ActorBase**)malloc(READY_INITIAL * sizeof(ActorBase*));
        schedulers[i].ready_count = 0;
        schedulers[i].ready_cap = schedulers[i].ready ? READY_INITIAL : 0;
        schedulers[i].ready_work = (ActorBase**)malloc(READY_INITIAL * sizeof(ActorBase*));
        schedulers[i].ready_work_count = 0;
        schedulers[i].ready_work_cap = schedulers[i].ready_work ? READY_INITIAL : 0;
        schedulers[i].ready_at = 0;
        schedulers[i].timers = (AetherTimer*)malloc(READY_INITIAL * sizeof(AetherTimer));
        schedulers[i].timer_count = 0;
        schedulers[i].timer_cap = schedulers[i].timers ? READY_INITIAL : 0;
        atomic_flag_clear_explicit(&schedulers[i].ready_in.lock, memory_order_relaxed);
        atomic_store_explicit(&schedulers[i].ready_in.count, 0, memory_order_relaxed);
        schedulers[i].ready_in.list = (ActorBase**)malloc(READY_INITIAL * sizeof(ActorBase*));
        schedulers[i].ready_in.cap = schedulers[i].ready_in.list ? READY_INITIAL : 0;
    }
    actor_park_init_once();
    atomic_store_explicit(&g_sched_state, 2, memory_order_release);
}

// The first spawn in a process whose main() never initialized the scheduler:
// a library's actors in a program without actors of its own, or a C host
// that calls into an Aether library (#2297). Initialize and start it the way
// a generated main() does. The program's own main(), when it has actors,
// initializes it before any spawn, so this is a single load on that path.
static void scheduler_init_on_demand(void) {
    if (atomic_load_explicit(&g_sched_state, memory_order_acquire) == 2) return;
    scheduler_init(cpu_recommend_cores());
    scheduler_start();
}

// Initialize with explicit optimization flags (TIER 3 opt-in)
void scheduler_init_with_opts(int cores, AetherOptFlags opts) {
    aether_runtime_configure(opts);
    scheduler_init(cores);
}

void scheduler_start(void) {
    // Only start threads once (idempotent: safe to call multiple times)
    if (atomic_exchange_explicit(&g_threads_started, 1, memory_order_acq_rel) == 1) {
        return;  // Threads already running
    }

    // Reset per this start() call in case scheduler_init was not called again.
    atomic_store_explicit(&g_threads_ready, 0, memory_order_relaxed);

    // The CPUs the threads are placed among, read before any of them binds
    // itself (#3106); pthread_create publishes them to the threads.
    read_process_cpus();

    for (int i = 0; i < num_cores; i++) {
        atomic_store_explicit(&schedulers[i].running, 1, memory_order_release);
        int rc = pthread_create(&schedulers[i].thread, NULL, scheduler_thread, &schedulers[i]);
        if (rc != 0) {
            fprintf(stderr, "ERROR: Failed to create scheduler thread for core %d: %d\n", i, rc);
        }
    }

    // Wait until every scheduler thread has finished its setup (pin_to_core,
    // send_buffer_init) and entered its main polling loop.  Without this
    // barrier, a caller that sends messages immediately after scheduler_start()
    // returns may enqueue to from_queues before the threads are running — the
    // messages sit unprocessed until the thread finally gets scheduled, which
    // can be hundreds of milliseconds later under OS thread-scheduling pressure
    // (commonly observed in test suites that create/join many threads).
    while (atomic_load_explicit(&g_threads_ready, memory_order_acquire) < num_cores) {
        AETHER_PAUSE();
    }
}

void scheduler_stop(void) {
    // Set all running flags to 0 with release semantics. `running` is the
    // word a core that is spinning reads on every pass of its loop, so this
    // store is what wakes it.
    //
    // It also rewrote each core's foreign queue tail with the value it had
    // just loaded, for an MWAIT on that word that no core makes. That store
    // took no foreign_lock, so a send from a thread the scheduler does not
    // own landing between the load and the store had the tail moved back
    // under it: its message lost, or slots past the tail delivered again
    // (#3521). Nothing reads the tail as a wake signal, so nothing writes it
    // here: taking the lock would have kept a store that wakes no one.
    for (int i = 0; i < num_cores; i++) {
        atomic_store_explicit(&schedulers[i].running, 0, memory_order_release);
    }

    // And wake anyone parked on the condvar, or shutdown waits out their
    // timeout for no reason (#1517). The actor threads of each core too
    // (#2592): they leave their loop once their core has stopped.
    for (int i = 0; i < num_cores; i++) {
        pthread_mutex_lock(&schedulers[i].park_mutex);
        pthread_cond_broadcast(&schedulers[i].park_cond);
        pthread_mutex_unlock(&schedulers[i].park_mutex);
        actor_park_broadcast(i);
    }
}

// Count total pending messages across all from_queues, TLS overflow buffers,
// AND the sent-vs-processed counter delta (catches messages sitting in actor
// mailboxes that aren't visible in queue/overflow counts).
// Called from the main thread during scheduler_wait().
static inline int count_pending_messages(void) {
    int total = 0;
    uint64_t total_sent = 0, total_proc = 0;
    /* Every core's handled count first, then every core's sent count: a
     * message is counted sent before its receiver can count it handled (the
     * count comes before the enqueue the receiver takes it from), so one
     * counted handled here is counted sent too, and one pending all along
     * counts. Read one core at a time, the two counts let a message sent on
     * a core already read and handled on one read after cancel the reply it
     * sent, which was still pending: the wait returned with it in flight
     * (std.stream's credits under UBSan). */
    // What threads that are no core's handled (#3541), with the cores'.
    total_proc += atomic_load_explicit(&g_foreign_processed, memory_order_seq_cst);
    for (int i = 0; i < num_cores; i++) {
        Scheduler* core = &schedulers[i];
        total_proc += atomic_load_explicit(&core->messages_processed, memory_order_seq_cst);
        for (int q = 0; q <= MAX_CORES; q++) {
            total += queue_size(&core->from_queues[q]);
        }
        // A death still to be finalized here (#2974): its Downs are not
        // sent yet.
        total += atomic_load_explicit(&core->control_pending, memory_order_acquire);
    }
    for (int i = 0; i < num_cores; i++)
        total_sent += atomic_load_explicit(&schedulers[i].messages_sent, memory_order_seq_cst);
    // Include main thread sends
    total_sent += atomic_load_explicit(&main_thread_sent, memory_order_seq_cst);
    // Include messages deferred in scheduler threads' TLS overflow buffers.
    int overflow = atomic_load_explicit(&g_overflow_total, memory_order_relaxed);
    if (overflow > 0) total += overflow;
    // sent > processed means messages are in actor mailboxes being processed.
    // This catches the ping-pong case where work-inlined messages bounce between
    // mailboxes without ever appearing in from_queues or overflow.
    if (total_sent > total_proc) {
        int64_t delta = (int64_t)(total_sent - total_proc);
        if (delta > 0 && delta < 100000000) {  // sanity bound
            total += (int)delta;
        }
    }
    return total;
}

// Wait for all pending messages to be processed (quiescence).
// Does NOT stop or join scheduler threads — they keep running and can
// process new messages sent after this function returns.
// Safe to call multiple times in a program (e.g. between test phases).
// Check if any actor has a pending timeout
/* Receive timeouts firing now (#3443): the generated step disarms its
 * countdown before the arm runs (codegen_actor.c, #3162), so from then on
 * the actor looks idle while its arm still runs, and may still send. A step
 * that fires a countdown is counted from before it starts until it returns,
 * and wait_for_idle() waits for it as for a message. */
static _Atomic int g_countdowns_firing = 0;
/* How many such steps ever returned: an arm that sent its last message and
 * returned inside a quiet window, after the window's counts, moves it on,
 * so the wait starts its window again (has_pending_actor_timeout alone read
 * the firing count once, at the window's end). */
static _Atomic uint64_t g_countdowns_done = 0;

void scheduler_countdown_enter(void) {
    atomic_fetch_add_explicit(&g_countdowns_firing, 1, memory_order_relaxed);
    /* Before the step's disarm, a plain store: a fence, which an RMW alone
     * is not for the stores after it (AArch64 without LSE). */
    atomic_thread_fence(memory_order_seq_cst);
}

void scheduler_countdown_leave(void) {
    atomic_fetch_add_explicit(&g_countdowns_done, 1, memory_order_release);
    atomic_fetch_sub_explicit(&g_countdowns_firing, 1, memory_order_release);
}

static int has_pending_actor_timeout(void) {
    int found = 0;
    int slot = reclaim_reader_enter();  // #2509
    for (int c = 0; c < num_cores && !found; c++) {
        int count;
        AetherActorTable* table = actor_table_snapshot(&schedulers[c], &count);
        for (int i = 0; i < count; i++) {
            ActorBase* a = actor_table_read(table, i);
            /* Armed, not merely configured: an `after` keeps its interval
             * once it fired, and is armed again only by a message (#3162).
             * A dead actor is never stepped, so its countdown never ends. */
            if (a && a->timeout_ns > 0 && a->last_activity_ns > 0 &&
                atomic_load_explicit(&a->dead, memory_order_relaxed) == 0) {
                found = 1;
                break;
            }
        }
    }
    reclaim_reader_exit(slot);
    /* A countdown seen disarmed above may be firing: its step counted itself
     * (a full barrier) before it disarmed. Acquire: a step seen done here
     * (its decrement read) has its count of the done read after it too, in
     * the caller's check, which a relaxed load let a weak memory read first
     * (AArch64, POWER). */
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&g_countdowns_firing, memory_order_acquire) > 0) found = 1;
    return found;
}

/* Until every message sent has been handled, and with `countdowns`, until
 * no live actor's receive timeout is armed either (each fires once its
 * deadline passes, and its arm may send more). wait_for_idle() waits for
 * both (#3102); the end of main for the messages alone (#3219). */
static void scheduler_wait_quiet(int countdowns);

/* wait_for_idle() waits for every actor, the calling one too, so inside an
 * actor's step (a handler, a receive timeout's arm) it waited for a step
 * that cannot end while it waits: forever in a handler, whose message stays
 * counted, and in an arm until the bound (#3463). So it did on any thread
 * that runs actors, after a step as much as in one: a core's, where a drop
 * or a death hook runs once the in-step flag is off and the step's message
 * not yet counted handled, and an actor's own. There it panics, as the
 * actor's code: the step barrier, or the death hook's or a contained drop's
 * own frame, takes it, and the actor's watchers are told; a drop run outside
 * any frame (a message to a dead actor) ends the process with the reason,
 * as any panic there does. */
AETHER_TLS int aether_in_actor_thread = 0;   /* set by aether_actor_thread.c */

void scheduler_wait(void) {
    if (g_aether_in_actor_step || current_core_id >= 0 || aether_in_actor_thread)
        aether_panic(AETHER_WAIT_IN_STEP_REASON);
    scheduler_wait_quiet(1);
}

static void scheduler_wait_quiet(int countdowns) {
    batch_flush_pending();
    // Check if scheduler is still running
    int still_running = 0;
    for (int i = 0; i < num_cores; i++) {
        if (atomic_load_explicit(&schedulers[i].running, memory_order_acquire)) {
            still_running = 1;
            break;
        }
    }
    if (!still_running) {
        return;  // Threads already stopped, nothing to wait for
    }

    // Wait until there are no pending messages in any from_queue or
    // overflow buffer.  We use a stability check: 5 consecutive reads
    // that return 0 with aether_usleep(100) between them (~500us total).
    // This gives scheduler threads time to finish any in-progress step()
    // calls and enqueue any resulting messages.
    int stable_count = 0;
    uint64_t last_processed = 0;
    int stall_checks = 0;
    int timeout_rounds = 0;
    uint64_t quiet_done = 0;   // countdown steps done when the window began (#3443)
    while (stable_count < 5) {
        atomic_thread_fence(memory_order_acquire);

        int pending = count_pending_messages();

        if (pending == 0) {
            if (stable_count == 0)
                quiet_done = atomic_load_explicit(&g_countdowns_done, memory_order_acquire);
            stable_count++;
            // Quiet, but a receive timeout is armed (#3102): its core fires
            // it once the deadline passes, and its arm may send more, so the
            // wait goes on, polling every millisecond (100000 times at most).
            // So it does when an arm ended inside the window, after the
            // counts that found it quiet (#3443).
            if (countdowns && stable_count == 5 && timeout_rounds < 100000 &&
                (has_pending_actor_timeout() ||
                 atomic_load_explicit(&g_countdowns_done, memory_order_acquire) != quiet_done)) {
                stable_count = 0;
                timeout_rounds++;
                aether_usleep(1000);
                continue;
            }
        } else {
            stable_count = 0;
        }

        // Periodic progress check (diagnostics only)
        stall_checks++;
#ifdef AETHER_DEBUG_ORDERING
        if (stall_checks % 2000 == 0) {
            uint64_t total_p = 0, total_s = 0;
            for (int i = 0; i < num_cores; i++) {
                total_p += schedulers[i].messages_processed;
                total_s += schedulers[i].messages_sent;
            }
            fprintf(stderr, "[WAIT %dk] pending=%d sent=%llu proc=%llu delta=%lld\n",
                    stall_checks/1000, pending,
                    (unsigned long long)total_s, (unsigned long long)total_p,
                    (long long)(total_s - total_p));
            fflush(stderr);
            if (total_p == last_processed && pending > 0) {
                fprintf(stderr, "  WARNING: no progress since last check!\n");
                fflush(stderr);
            }
            last_processed = total_p;
        }
#else
        (void)last_processed;
        (void)stall_checks;
#endif

        // Adaptive wait: spin for fast drain, usleep only for bulk.
        // usleep() has ~10µs minimum latency on macOS; during the
        // convergence phase (pending < 10k), this dominates wall time.
        if (pending <= 10000) {
            for (int sp = 0; sp < 200; sp++) AETHER_PAUSE();
            aether_sched_yield();
        } else {
            aether_usleep(100);  // 100 microseconds
        }
    }
}

// Full shutdown: wait for quiescence, stop threads, join them.
// Called once at program exit.  Safe to call even if threads were never
// started or already shut down.
void scheduler_shutdown(void) {
    /* The messages already sent are handled; a receive timeout still armed
     * is not waited for (#3219). The program is over, so the silence the
     * countdown waits on will not be broken, and an actor with `after
     * 60000` that handled a message just before main returned held the exit
     * for that minute and then ran its arm at shutdown. wait_for_idle()
     * still waits for countdowns: a program that wants one to fire says so.
     * The cooperative scheduler ends the same way. */
    scheduler_wait_quiet(0);

    // Stopping (3): a send from a thread the scheduler does not own is
    // refused from here on, not queued for cores about to stop (#2912).
    int running = 2;
    atomic_compare_exchange_strong(&g_sched_state, &running, 3);
    wait_for_foreign_senders();

    // Signal threads to stop
    scheduler_stop();

    // Join threads — but only once.
    // pthread_join on an already-joined thread is undefined behaviour (crash on Linux).
    if (atomic_exchange_explicit(&g_threads_joined, 1, memory_order_acq_rel) == 0) {
        for (int i = 0; i < num_cores; i++) {
            if (schedulers[i].thread) {
                int result = pthread_join(schedulers[i].thread, NULL);
                (void)result;  // Suppress unused warning
            }
        }

        // Cleanup NUMA resources
        aether_numa_cleanup();
    }

    // The core threads are gone; free what no other walker holds (#2509).
    actor_reclaim(1);

    /* Write the trace here, not from atexit: every scheduler thread has been
     * joined by this point, so the per-core buffers are quiescent and the join
     * supplies the happens-before the merge relies on. It also keeps the exit
     * path free of the hang the worker pool hit when it tried to do teardown
     * work from atexit. Compiles to nothing without -DAETHER_TRACE. */
    AETHER_TRACE_FLUSH();

    // A stopped scheduler can be initialized again.
    atomic_store_explicit(&g_sched_state, 0, memory_order_release);
}

// Free the per-core tables scheduler_init allocated: the actor slots, the
// I/O poller and its fd map, the park and foreign-send locks.
// scheduler_cleanup() and a scheduler_init() that follows a
// scheduler_shutdown() both release them this way.
static void scheduler_free_core_tables(void) {
    // g_sched_state is not running here (init has claimed it, or cleanup
    // cleared it); a foreign sender still on its way out finishes first.
    wait_for_foreign_senders();
    // Actors never released (a program or host that stops the scheduler
    // with actors alive, a panicked actor) are reachable only through the
    // tables freed here. Those the scheduler spawned end the way a release
    // ends them; the core threads are joined and no reader is left. One a
    // caller registered stays the caller's. Each table is scanned and taken
    // away under its core's lock: an actor's own thread can be ending its
    // actor meanwhile (scheduler_actor_thread_exit), and its deregister,
    // which takes the same lock, then finds the table whole or gone.
    for (int i = 0; i < num_cores; i++) {
        Scheduler* sched = &schedulers[i];
        spinlock_lock(&sched->actor_lock);
        AetherActorTable* table = atomic_load_explicit(&sched->actor_table, memory_order_relaxed);
        int count = atomic_load_explicit(&sched->actor_count, memory_order_relaxed);
        for (int k = 0; table && k < count; k++) {
            ActorBase* actor = atomic_load_explicit(&table->slots[k], memory_order_relaxed);
            if (!actor) continue;
            if (!actor->scheduler_owned) {
                // A caller's actor outlives this lifecycle, and its count of
                // messages in flight (#3090) starts again from none with the
                // queues that go with it: no sender is counting one now (a
                // send is refused while the scheduler is not running, and
                // the last foreign one has finished, above).
                atomic_store_explicit(&actor->inflight, 0, memory_order_relaxed);
                // In no ready list or timer heap: they go with the tables
                // (#3540). And the core it is on stays its core (#3673): a
                // send made after the next scheduler_init and before its
                // registration goes there, and that core delivers it, so
                // the registration keeps it there (AETHER_EXIT_ROUTED).
                atomic_store_explicit(&actor->ready_queued, 0, memory_order_relaxed);
                atomic_store_explicit(&actor->timer_slot, 0, memory_order_relaxed);
                atomic_fetch_or_explicit(&actor->exit_flags, AETHER_EXIT_ROUTED, memory_order_release);
                // Its overflow (#3241) is the runtime's and goes with the
                // queues, what it holds dropped as a release drops it: the
                // caller frees the actor's memory, which is all it knows of.
                if (!actor->auto_process) {
                    ActorInbox* ib = atomic_exchange_explicit(&actor->inbox, NULL,
                                                              memory_order_acq_rel);
                    if (ib) {
                        Message spill[AETHER_INBOX_RING];
                        int got;
                        while ((got = actor_inbox_take(ib, spill, AETHER_INBOX_RING)) > 0)
                            for (int m = 0; m < got; m++) release_undelivered(actor, &spill[m]);
                        free(ib);
                    }
                }
                aether_watch_free(actor);   // its death notices (#2974)
                continue;
            }
            if (actor->auto_process) {
                // Its own thread may still be running it (#2517): this is a
                // release, and the second of it and the thread's exit ends
                // the actor. Its block is freed here only when the thread
                // has already left.
                int was = atomic_fetch_or_explicit(&actor->dead, AETHER_ACTOR_RELEASED,
                                                   memory_order_acq_rel);
                if (!(was & AETHER_ACTOR_THREAD_GONE)) continue;
            }
            actor_free_now(actor);
        }
        atomic_store_explicit(&sched->actor_table, NULL, memory_order_release);
        atomic_store_explicit(&sched->actor_count, 0, memory_order_relaxed);
        spinlock_unlock(&sched->actor_lock);
        // An actor thread asleep in its park sees the mark set above now,
        // not at the end of its timed wait (#2592).
        actor_park_broadcast(i);
        // The live table and every table it replaced, each with its own size:
        // a grown table is bigger than MAX_ACTORS_PER_CORE slots (#2486).
        actor_table_free_chain(table);
    }
    // Released actors not reclaimed yet: no reader is left either (#2509).
    // Then the kept blocks go back to the allocator (#2517).
    actor_reclaim_all();
    tombstone_free_all();
    for (int i = 0; i < num_cores; i++) {
        // Clean up thread resources
        schedulers[i].thread = 0;

        // The core's park and foreign-send locks live for one lifecycle:
        // scheduler_init() makes them and they end here, with the core's
        // thread joined. On Windows a critical section that has once been
        // contended holds a heap block (its 48-byte debug record) until
        // DeleteCriticalSection, so making them again without this lost a
        // block for each lock contended in the lifecycle: a wake that met
        // the core at its park, two threads off the cores sending at once.
        pthread_mutex_destroy(&schedulers[i].park_mutex);
        pthread_cond_destroy(&schedulers[i].park_cond);
        pthread_mutex_destroy(&schedulers[i].foreign_lock);
        // Finalize requests left at the stop: the actors end with the tables.
        free(schedulers[i].control);
        schedulers[i].control = NULL;
        schedulers[i].control_count = 0;
        schedulers[i].control_cap = 0;
        atomic_store_explicit(&schedulers[i].control_pending, 0, memory_order_relaxed);
        // The ready list and timer heap (#3540): their actors went with the
        // tables above; a caller's actor registered again starts with no
        // entry (scheduler_register_actor).
        free(schedulers[i].ready);
        free(schedulers[i].ready_work);
        free(schedulers[i].timers);
        free(schedulers[i].ready_in.list);
        schedulers[i].ready = NULL;
        schedulers[i].ready_work = NULL;
        schedulers[i].timers = NULL;
        schedulers[i].ready_in.list = NULL;
        schedulers[i].ready_count = schedulers[i].ready_cap = 0;
        schedulers[i].ready_work_count = schedulers[i].ready_work_cap = schedulers[i].ready_at = 0;
        schedulers[i].timer_count = schedulers[i].timer_cap = 0;
        schedulers[i].ready_in.cap = 0;
        atomic_store_explicit(&schedulers[i].ready_in.count, 0, memory_order_relaxed);

        // Clean up I/O event loop
        aether_io_poller_destroy(&schedulers[i].io_poller);
        free(schedulers[i].io_map);
        schedulers[i].io_map = NULL;
        schedulers[i].io_registered_count = 0;
    }
}

void scheduler_cleanup(void) {
    atomic_store(&g_sched_state, 0);
    // Free allocated scheduler resources
    scheduler_free_core_tables();
    num_cores = 0;
}

static int register_actor(ActorBase* actor, int preferred_core);
static int register_actor_ex(ActorBase* actor, int preferred_core, int expect, int check);

// An actor the caller allocated: the scheduler runs it and never frees it.
// Its count of messages in flight is left as it is: a caller's actor can be
// registered again after a shutdown and a scheduler_init() while a thread of
// the caller's is sending to it, and a message counted for it before the
// registration is still on its way. Set to 0 here, as it used to be, the
// delivery of that message took the count to -1, which reads as a move in
// progress, and the next sender waited for it forever (#3090). The count
// starts at 0 in a zeroed actor, and teardown sets it back to 0
// (scheduler_free_core_tables).
int scheduler_register_actor(ActorBase* actor, int preferred_core) {
    actor->scheduler_owned = 0;
    actor->release_message = NULL;   // its messages are the caller's (#3142)
    actor->step_payload = NULL;
    /* Stores, not atomic_init: a caller's actor can be registered again while
     * a thread of the caller's is sending to it (above), and every send reads
     * the bound and its count. atomic_init is a plain store, which raced
     * those atomic loads. */
    atomic_store_explicit(&actor->thread_parked, 0, memory_order_relaxed);
    atomic_store_explicit(&actor->death_watch, NULL, memory_order_relaxed);         // #2974
    atomic_store_explicit(&actor->incarnation, 0, memory_order_relaxed);
    atomic_store_explicit(&actor->mailbox_bound, 0, memory_order_relaxed);      // the generated spawn sets a bound (#2974)
    atomic_store_explicit(&actor->mailbox_pending, 0, memory_order_relaxed);
    atomic_store_explicit(&actor->full_drops, 0, memory_order_relaxed);
    /* An actor a core may already be delivering to keeps that core (#3673):
     * one carried over a restart, whose messages since scheduler_init went
     * to the core its last lifecycle left in assigned_core, and one a core
     * took because its assigned_core named none (AETHER_EXIT_ROUTED, set
     * before that core stored its id). That core is its mailbox's one
     * writer; giving the actor another core here, as the registration did,
     * had both write it, the second while the first was still delivering.
     * It goes into the table of the core it is on, and a core the caller
     * asked for is asked of that core as a move (migrate_to), which hands
     * the actor over with nothing of it in flight, as every move does
     * (#3090). Read in this order, against a core's take of it (the bit,
     * then its id): a core id read here that a take stored has the bit
     * with it. */
    int core;
    for (;;) {
        int cur = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
        int routed = atomic_load_explicit(&actor->exit_flags, memory_order_acquire) & AETHER_EXIT_ROUTED;
        if (routed && cur >= 0 && cur < num_cores) {
            core = register_actor_ex(actor, cur, 0, 0);
            atomic_store_explicit(&actor->exit_flags, 0, memory_order_release);
            if (core >= 0 && preferred_core >= 0 && preferred_core < num_cores && preferred_core != core)
                atomic_store_explicit(&actor->migrate_to, preferred_core, memory_order_relaxed);
            break;
        }
        // No core delivers to it (a new actor) or none can yet (its core is
        // past this lifecycle's count): placed as any actor is, unless a
        // core takes it first, which the compare-and-swap sees.
        if (!routed) {
            // In no list (#3540) and with no overflow: those of a
            // lifecycle it was registered in before went with its tables.
            atomic_init(&actor->inbox, NULL);
            atomic_init(&actor->ready_queued, 0);
            atomic_init(&actor->timer_slot, 0);
        }
        core = register_actor_ex(actor, preferred_core, cur, 1);
        if (core == -2) continue;   // a core took it meanwhile: it keeps it
        atomic_store_explicit(&actor->exit_flags, 0, memory_order_release);
        break;
    }
    /* Its core looks at it once now (#3540), as the scan looked at every
     * actor of its table on its next pass: the ready list brings a core
     * only to the actors it is told of. A countdown armed before is found
     * this way (one carried over a restart lost its heap entry with the
     * last lifecycle's tables; one armed before the registration was told
     * when it was on no core, and the push went nowhere), as are messages
     * already in its mailbox and a move asked of it. */
    ready_push(actor);
    return core;
}
static int register_actor(ActorBase* actor, int preferred_core) {
    return register_actor_ex(actor, preferred_core, 0, 0);
}

/* With `check`, assigned_core is set only if it still reads `expect` (a
 * compare-and-swap, under the core's lock): -2 when a core took the actor
 * meanwhile (#3673), and nothing is changed. */
static int register_actor_ex(ActorBase* actor, int preferred_core, int expect, int check) {
    // Default placement: pick the worker with the lowest combined
    // load right now. The previous shape — `actor_id % num_cores` —
    // gave perfect distribution only on long actor sequences; on
    // small actor counts (e.g. a two-actor site-poller) two ids
    // reliably collide on the same core and the second actor
    // starves while the first runs a long handler.
    //
    // The scan is O(num_cores) of relaxed atomic loads, sub-microsecond
    // on every reasonable box. The user-supplied `core: N` override
    // skips it. Combining work_count (in-flight messages) and
    // actor_count (total actors assigned) reflects both queue
    // depth AND lifetime load; ties broken by lowest core index for
    // deterministic placement under cold-start.
    //
    // Strictly better than the modulo: same-or-better distribution on
    // small N, identical on large N (cores fill evenly anyway).
    if (preferred_core < 0) {
        if (num_cores <= 1) {
            preferred_core = 0;
        } else {
            int target = 0;
            long best  = LONG_MAX;
            for (int c = 0; c < num_cores; c++) {
                long load = (long)atomic_load_explicit(
                                &schedulers[c].work_count,
                                memory_order_relaxed)
                          + (long)atomic_load_explicit(
                                &schedulers[c].actor_count,
                                memory_order_relaxed);
                if (load < best) {
                    best = load;
                    target = c;
                }
            }
            preferred_core = target;
        }
    }

    Scheduler* sched = &schedulers[preferred_core];

    spinlock_lock(&sched->actor_lock);

    // A full table is replaced by one twice its size, published before the
    // count that needs it; the old one stays for readers still walking it.
    if (!actor_table_has_room_locked(sched) &&
        actor_table_grow_locked(sched, aether_numa_node_of_cpu(preferred_core)) != 0) {
        spinlock_unlock(&sched->actor_lock);
        fprintf(stderr, "Fatal: Failed to grow actor array for core %d\n", preferred_core);
        return -1;
    }

    if (check) {
        int seen = expect;
        if (!atomic_compare_exchange_strong_explicit(&actor->assigned_core, &seen, preferred_core,
                                                     memory_order_acq_rel, memory_order_acquire)) {
            spinlock_unlock(&sched->actor_lock);
            return -2;
        }
    } else {
        atomic_store_explicit(&actor->assigned_core, preferred_core, memory_order_relaxed);
    }

    // The SPSC queue and an actor thread's inbox are both made on first use
    // (saves 3 KB/actor).

    actor_table_push_locked(sched, actor);

    spinlock_unlock(&sched->actor_lock);

    return preferred_core;
}

// Remove an actor from its scheduler's actor array.
// Must be called before freeing actor memory to avoid dangling pointers.
// assigned_core is read again under the core's lock (#2509). Every move to
// another core (migration, work stealing) stores it with the source core's
// lock held, so while it names the locked core the actor is in that core's
// table and stays there. Read before the lock, it could name a core the actor
// had just left, and the removal missed it.
/* Out of its core's table. Returns 0 when it is on no core. */
static int actor_table_leave(ActorBase* actor) {
    for (;;) {
        int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
        if (core < 0 || core >= num_cores) return 0;

        Scheduler* sched = &schedulers[core];
        spinlock_lock(&sched->actor_lock);
        if (atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == core) {
            int at = actor_table_find_locked(sched, actor, -1);
            if (at >= 0) actor_table_remove_locked(sched, at);
            spinlock_unlock(&sched->actor_lock);
            return 1;
        }
        spinlock_unlock(&sched->actor_lock);
    }
}

/* Whether no thread runs core `core`'s loop: its ready lists and timer heap
 * are then any thread's to change. */
static int core_loop_quiet(int core) {
    return !atomic_load_explicit(&g_threads_started, memory_order_acquire) ||
           atomic_load_explicit(&schedulers[core].loop_left, memory_order_acquire);
}

/* Until no core's list holds an entry for `actor` and no heap its countdown,
 * taking them out itself from the cores no thread runs. */
static void ready_forget_wait(ActorBase* actor) {
    for (unsigned spin = 0;
         atomic_load_explicit(&actor->ready_queued, memory_order_acquire) != 0 ||
         atomic_load_explicit(&actor->timer_slot, memory_order_acquire) != 0;
         spin++) {
        if ((spin & 255) == 0) {
            for (int k = 0; k < num_cores; k++)
                if (core_loop_quiet(k) && ready_forget_on(&schedulers[k], actor))
                    atomic_store_explicit(&actor->ready_queued, 0, memory_order_release);
        }
        if (spin > 64) aether_sched_yield();
        else AETHER_PAUSE();
    }
}

/* A deregistered actor out of every ready list and timer heap (#3540), and
 * looked at by its core no more: nothing of the scheduler reads it once this
 * returns, and its caller may free it, as with the scan, which looked only
 * at its table. Marked first (AETHER_EXIT_DEREGISTERED), so its core drops
 * the entries it takes for it and fires no countdown of it. Then a first
 * wait, for an entry that may be in another core's list (one pushed before
 * a move, which that core passes on), and a fresh entry on its core, which
 * its core takes after anything it was doing with the actor: a step, a
 * countdown. On its core's own thread (a step deregistering an actor of its
 * core) its lists are this thread's, and it takes it out of them itself. A
 * core that no thread runs, it takes it out of itself. A thread that runs
 * another core waits for the actor's core: deregistering the actor of a
 * core that waits on this one (an ask) waits until that ask ends. */
static void ready_forget(ActorBase* actor) {
    if (actor->auto_process) return;   // in no core's lists: its own thread runs it
    atomic_fetch_or_explicit(&actor->exit_flags, AETHER_EXIT_DEREGISTERED, memory_order_acq_rel);
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    if (core < 0 || core >= num_cores) return;
    if (t_core == core) {
        if (ready_forget_on(&schedulers[core], actor))
            atomic_store_explicit(&actor->ready_queued, 0, memory_order_release);
        ready_forget_wait(actor);
        return;
    }
    ready_forget_wait(actor);
    ready_push(actor);
    ready_forget_wait(actor);
}

void scheduler_deregister_actor(ActorBase* actor) {
    if (!actor) return;
    if (actor_table_leave(actor)) ready_forget(actor);
}

// ---------------------------------------------------------------------------
// I/O event integration (platform-agnostic: epoll/kqueue/poll)
// ---------------------------------------------------------------------------

// Grow io_map to accommodate fd. Returns 0 on success, -1 on failure.
static int scheduler_io_map_grow(Scheduler* sched, int fd) {
    // Start from at least 1 so a zero capacity (initial map allocation failed)
    // grows normally instead of looping forever on new_cap *= 2.
    int new_cap = sched->io_map_capacity > 0 ? sched->io_map_capacity : 1;
    while (new_cap <= fd) new_cap *= 2;

    AetherIoEntry* new_map = realloc(sched->io_map, (size_t)new_cap * sizeof(AetherIoEntry));
    if (!new_map) return -1;

    // Zero-initialize the new entries
    memset(&new_map[sched->io_map_capacity], 0,
           (size_t)(new_cap - sched->io_map_capacity) * sizeof(AetherIoEntry));
    sched->io_map = new_map;
    sched->io_map_capacity = new_cap;
    return 0;
}

int scheduler_io_register(int core_id, int fd, void* actor, uint32_t events) {
    if (core_id < 0 || core_id >= num_cores) return -1;
    if (fd < 0) return -1;

    Scheduler* sched = &schedulers[core_id];

    // Grow io_map if fd exceeds current capacity
    if (fd >= sched->io_map_capacity) {
        if (scheduler_io_map_grow(sched, fd) != 0) return -1;
    }

    if (aether_io_poller_add(&sched->io_poller, fd, actor, events) != 0)
        return -1;

    sched->io_map[fd].fd = fd;
    sched->io_map[fd].actor = actor;
    sched->io_map[fd].events = events;
    if (!sched->io_map[fd].active) {
        sched->io_map[fd].active = 1;
        sched->io_registered_count++;
    }
    return 0;
}

void scheduler_io_unregister(int core_id, int fd) {
    if (core_id < 0 || core_id >= num_cores) return;
    if (fd < 0 || fd >= schedulers[core_id].io_map_capacity) return;

    Scheduler* sched = &schedulers[core_id];
    aether_io_poller_remove(&sched->io_poller, fd);

    if (sched->io_map[fd].active) {
        sched->io_map[fd].active = 0;
        sched->io_map[fd].actor = NULL;
        sched->io_registered_count--;
    }
}

// Drain I/O events for a scheduler core, delivering MSG_IO_READY messages.
// Called from the scheduler's main loop. Returns number of events processed.
static int scheduler_io_poll(Scheduler* sched, int timeout_ms) {
    if (sched->io_registered_count == 0) return 0;

    AetherIoEvent events[64];
    int n = aether_io_poller_poll(&sched->io_poller, events, 64, timeout_ms);
    if (n <= 0) return 0;

    for (int i = 0; i < n; i++) {
        int fd = events[i].fd;
        if (fd < 0 || fd >= sched->io_map_capacity) continue;

        AetherIoEntry* entry = &sched->io_map[fd];
        if (!entry->active || !entry->actor) continue;

        ActorBase* actor = (ActorBase*)entry->actor;

        // Build and deliver MSG_IO_READY directly to mailbox
        IoReadyMessage io_msg;
        io_msg.type = MSG_IO_READY;
        io_msg.fd = fd;
        io_msg.events = events[i].events;

        Message msg;
        msg.type = MSG_IO_READY;
        msg.sender_id = 0;
        msg.payload_int = fd;

        // Allocate payload copy for mailbox ownership
        void* payload = malloc(sizeof(IoReadyMessage));
        if (!payload) continue;
        memcpy(payload, &io_msg, sizeof(IoReadyMessage));
        msg.payload_ptr = payload;
        msg.zerocopy.data = NULL;
        msg.zerocopy.size = 0;
        msg.zerocopy.owned = AETHER_MSG_ZC_BORROWED;
        msg._reply_slot = NULL;

        // One-shot: the backend auto-disables the fd after firing.
        // Mark our io_map entry inactive so we don't try to unregister again.
        entry->active = 0;
        entry->actor = NULL;
        sched->io_registered_count--;

        // Route through the scheduler's self-channel (from_queues[core_id])
        // instead of mailbox_send. The mailbox is SPSC and the accept thread
        // is already the producer via direct_send — adding a second producer
        // (this scheduler thread) would corrupt the ring buffer. Into the
        // queues: counted as every such message is (#3090).
        if (!actor->auto_process) inflight_enter(actor, 1);
        requeue_own(sched->core_id, actor, msg);
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    }
    return n;
}

// Thread-local recursion guard for work inlining (prevent stack overflow)
static AETHER_TLS int inline_depth = 0;
#define MAX_INLINE_DEPTH 2

static void send_local_admitted(ActorBase* actor, Message msg);
static void send_remote_admitted(ActorBase* actor, Message msg, int from_core);

/* A send's way in (#2974): a bounded actor's place is taken first, so every
 * drop on the way, a dead target's included, gives one back. The bound is
 * on the line `dead` is on. */
void scheduler_send_local(ActorBase* actor, Message msg) {
    if (unlikely(actor && atomic_load_explicit(&actor->mailbox_bound, memory_order_relaxed)) &&
        !bound_admits(actor, &msg))
        return;
    send_local_admitted(actor, msg);
}

void scheduler_send_remote(ActorBase* actor, Message msg, int from_core) {
    if (unlikely(actor && atomic_load_explicit(&actor->mailbox_bound, memory_order_relaxed)) &&
        !bound_admits(actor, &msg))
        return;
    send_remote_admitted(actor, msg, from_core);
}

const char* scheduler_try_send(ActorBase* actor, Message msg) {
    if (!actor || atomic_load_explicit(&actor->dead, memory_order_acquire)) {
        aether_message_drop(actor, &msg);
        return "the actor is dead";
    }
    if (!aether_mailbox_admit(actor, msg.type)) {
        aether_message_drop(actor, &msg);
        return "the mailbox is full";
    }
    int me = current_core_id;
    if (me >= 0 && me == atomic_load_explicit(&actor->assigned_core, memory_order_relaxed))
        send_local_admitted(actor, msg);
    else
        send_remote_admitted(actor, msg, me);
    return "";
}

static void send_local_admitted(ActorBase* actor, Message msg) {
    AETHER_TRACE_EVENT(AE_TRACE_SEND_LOCAL, actor ? actor->id : 0,
                       msg.type, msg.sender_id);
    // Drop messages to dead actors: they can't process them and we don't
    // want to grow their mailbox or fire their step(). A released actor's
    // block is kept, so this read is defined after its release (#2517).
    if (unlikely(!actor || atomic_load_explicit(&actor->dead, memory_order_acquire))) {
        send_to_dead_actor(actor, &msg);
        return;
    }

    /* Only the actor's own core thread writes its mailbox: a send from any
     * other thread (a C host's main thread, a core thread sending to an
     * actor of another core) goes the way scheduler_send_remote takes it,
     * through the target core's queues. Written here, as it was from a
     * thread that is no core's, it was a second producer on the mailbox's
     * single producer ring, beside the core thread's own deliveries: a
     * message overwrote another or was taken twice. Acquire: a core that has
     * just been handed the actor (move_actor's release store of its id)
     * writes the mailbox only once it sees what the core that had it wrote
     * there, its tail and the count, last; read relaxed, its write raced
     * the other core's last delivery. */
    if (unlikely(!actor->auto_process &&
                 (current_core_id < 0 ||
                  current_core_id != atomic_load_explicit(&actor->assigned_core,
                                                          memory_order_acquire)))) {
        send_remote_admitted(actor, msg, current_core_id);
        return;
    }

    // NOTE: Inline mode optimization is handled by the existing WORK INLINING
    // code below (lines 590+), which is safe because it checks for scheduler
    // thread context (current_core_id >= 0 && actor->assigned_core == current_core_id).
    //
    // The profile-based inline mode (aether_inline_mode_active) is NOT used here
    // because it would cause races when main thread sends to actors being
    // processed by scheduler threads.

    // Counted sent: a core's thread on its own counter, with no atomic add
    // (#3541), any other thread on the shared one.
    count_sent(1);
    // An actor with its own thread owns its mailbox: the message goes in its
    // inbox (#2598).
    if (unlikely(actor->auto_process)) {
        scheduler_actor_thread_deliver(actor, msg);
    } else {
        AETHER_TRACE_EVENT(AE_TRACE_MAILBOX_SEND, actor->id, msg.type, msg.sender_id);
        // Set active=1 BEFORE the mailbox_send count++ (release).
        // mailbox_send does atomic_fetch_add(&count, release), which publishes
        // both the message data AND this active=1 to any thread that subsequently
        // reads count with acquire (mailbox_receive or the scheduler loop's
        // acquire pre-check).  This is the cross-thread happens-before required
        // for the work-stealing handoff: if the actor is stolen to another core
        // after this send, the new owner will see both active=1 and the message.
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
        // Straight into the mailbox only when nothing of the actor's is in
        // the queues (#3093): a message of it waiting in this core's queue,
        // its mailbox full when it was sent, is older, and one put in the
        // mailbox past it was stepped first. Otherwise queued behind it.
        // With nothing queued, behind what its overflow holds (#3241).
        int queued = atomic_load_explicit(&actor->inflight, memory_order_relaxed) != 0;
        if (unlikely(queued || !deliver_owned(actor, msg))) {
            // Mailbox full: re-queue via self-channel or overflow buffer.
            // Without this, the message is lost but messages_sent was already
            // incremented, causing scheduler_wait to hang forever. This is
            // the actor's core thread (above). Into the queues: counted as
            // every such message is (#3090).
            inflight_enter(actor, 1);
            requeue_own(current_core_id, actor, msg);
        }
    }

    // WORK INLINING: If actor is idle and we're not too deep, run it immediately.
    // This eliminates scheduler loop overhead for tight request-response patterns.
    // Re-check assigned_core with relaxed: if work-stealing fired between the
    // mailbox write and here, assigned_core will have changed and we skip inline.
    //
    // When overflow sends are pending, still allow inlining (to make progress)
    // but flush overflow between inline calls to prevent unbounded growth.
    //
    // Never an actor with its own thread: that thread steps it, without the
    // step_lock, and its mailbox is the thread's alone. Its count can read 1
    // here when the thread has just moved a message in and is about to step,
    // and stepping it here as well ran its handler on two threads at once
    // (#2598).
    if (likely(current_core_id >= 0) &&
        !actor->auto_process &&
        inline_depth < MAX_INLINE_DEPTH &&
        atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == current_core_id &&
        atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) == 1 &&
        !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
        inline_depth++;
        count_processed((uint64_t)aether_step_safe(actor));
        int left = atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0;
        if (!left) {
            atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
        }
        // Flush overflow accumulated during step() before next inline.
        // This prevents unbounded overflow growth from recursive inlining.
        if (tls_overflow_any) {
            overflow_flush(current_core_id);
        }
        inline_depth--;
        atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
        // Stepped and still holding one: its core's ready list (#3540).
        if (left) ready_push(actor);
    } else if (!actor->auto_process &&
               atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0) {
        // Not stepped here (it is stepping, the inline depth is spent, a
        // message of it was already waiting): its core's ready list brings
        // the loop to it (#3540). This is its core's thread, so a push
        // already made costs one load.
        ready_push(actor);
    }
}

static void send_through_core(ActorBase* actor, Message msg, int from_core);

static void send_remote_admitted(ActorBase* actor, Message msg, int from_core) {
    AETHER_TRACE_EVENT(AE_TRACE_SEND_REMOTE, actor ? actor->id : 0,
                       msg.type, msg.sender_id);
    // Drop messages to dead actors (see scheduler_send_local).
    if (unlikely(!actor || atomic_load_explicit(&actor->dead, memory_order_acquire))) {
        send_to_dead_actor(actor, &msg);
        return;
    }
    // An actor with its own thread takes the message in its inbox, from any
    // thread (#2598): not through a core, which would only pass it on, and
    // never stepped inline here, which would run it on a second thread.
    if (unlikely(actor->auto_process)) {
        count_sent(1);
        scheduler_actor_thread_deliver(actor, msg);
        return;
    }
    if (current_core_id >= 0) {
        send_through_core(actor, msg, from_core);
        return;
    }
    batch_flush_pending();
    if (!foreign_send_enter()) {
        send_while_stopped(actor, &msg);
        return;
    }
    send_through_core(actor, msg, from_core);
    foreign_send_leave();
}

static void send_through_core(ActorBase* actor, Message msg, int from_core) {
    // Counted sent: a core's thread on its own counter, with no atomic add
    // (#3541), any other thread on the shared one.
    count_sent(1);
    // Acquire, as in send_local_admitted (#3683): the direct delivery below
    // writes the mailbox when this core has the actor, after the core that
    // had it before moved it here with a release store of the id.
    int target_core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);

    // Guard against uninitialized or invalid assigned_core
    if (unlikely(target_core < 0 || target_core >= num_cores)) {
        target_core = actor->id % num_cores;
    }


    // Already same-core AND caller is actually running on that core's
    // scheduler thread — deliver directly to mailbox (no queue overhead).
    // The current_core_id check prevents non-scheduler threads (e.g. main)
    // from racing with the scheduler thread on mailbox access. Not counted:
    // only this core moves the actor, and not while it is sending.
    if (from_core >= 0 && from_core == target_core &&
        from_core == current_core_id) {
        // Behind anything of the actor's already in the queues, as in
        // scheduler_send_local (#3093), or in its overflow (#3241).
        if (unlikely(atomic_load_explicit(&actor->inflight, memory_order_relaxed) != 0 ||
                     !deliver_owned(actor, msg))) {
            // Mailbox full: re-queue via self-channel or overflow, counted
            // as every message in the queues is.
            inflight_enter(actor, 1);
            requeue_own(from_core, actor, msg);
        } else {
            ready_push(actor);   // nothing steps it here (#3540)
        }
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
        AETHER_STAT_INC(direct_sends);
        return;
    }

    // Through a core's queues (#3090, #3093): counted before the actor's core
    // is read again, so the actor cannot move from the core this message is
    // routed to before the message reaches its mailbox there.
    if (!actor->auto_process) {
        inflight_enter(actor, 1);
        /* Its target's death is read again now that it is counted (#2974):
         * a send takes effect at this read. One counted before a death is
         * dropped where it arrives, and an actor with a message counted is
         * not reborn (scheduler restart), so none reaches a later
         * incarnation; one counted after it drops itself here. */
        if (unlikely(atomic_load_explicit(&actor->dead, memory_order_acquire) != 0)) {
            send_to_dead_actor(actor, &msg);
            inflight_leave(actor, 1);   // after the drop, which reads it
            count_processed(1);
            return;
        }
        target_core = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
        if (unlikely(target_core < 0 || target_core >= num_cores)) {
            target_core = actor->id % num_cores;
        }
    }

    // A sender on another core asks for the actor (message-driven
    // migration): its core moves it here before it runs this message, when
    // no other message of it is in flight (scheduler_thread, move_actor), so
    // communicating actors gather on one core and their sends turn local. A
    // ring gathers in its first lap. Asked for only once it had run, as it
    // used to be, an actor in a chain was never asked: the next one is idle
    // when the message reaches it (#3090). Not for an ask (#3196): the
    // asking core runs nothing until the reply comes, so its target moved
    // there could not run to give it.
    if (from_core >= 0 && from_core == current_core_id && !actor->auto_process &&
        from_core != target_core && !msg._reply_slot) {
        atomic_store_explicit(&actor->migrate_to, from_core, memory_order_relaxed);
    }

    // Enqueue to target core's per-sender SPSC channel (SPSC: only current_core_id writes here).
    int from_idx = (current_core_id >= 0 && current_core_id < MAX_CORES) ? current_core_id : MAX_CORES;

    // Not a scheduler thread: the main thread, or a foreign C thread (a
    // std.http pool worker, a std.worker thread, a library callback). They
    // all share the [MAX_CORES] channel, whose SPSC producer side tolerates
    // exactly one writer, so they take the channel's lock and enqueue under
    // it (#2083: two such threads writing the slot at once lost messages
    // with no diagnostic). Spinning here is safe — none of these threads
    // drains from_queues, so it cannot be part of a circular wait — and a
    // foreign thread has no TLS overflow buffer anyone would ever flush.
    if (from_idx == MAX_CORES) {
        pthread_mutex_lock(&schedulers[target_core].foreign_lock);
        int retries = 0;
        while (!queue_enqueue(&schedulers[target_core].from_queues[MAX_CORES], actor, msg)) {
            if (++retries % 1000 == 0) aether_sched_yield();
            AETHER_PAUSE();
        }
        pthread_mutex_unlock(&schedulers[target_core].foreign_lock);
        sched_wake(&schedulers[target_core]);
        AETHER_STAT_INC(queue_sends);
        return;
    }

    // Ordering invariant: if any messages are already pending for this target core,
    // we must defer this one too — otherwise it would arrive before the pending ones.
    if (from_core >= 0 && unlikely(tls_overflow[target_core].count > 0)) {
        overflow_append(target_core, actor, msg);
        AETHER_STAT_INC(queue_sends);
        return;
    }

    // Bounded retry: a short spin is fine; an indefinite spin inside the scheduler
    // loop causes back-pressure deadlock (all threads block, nobody drains queues).
    for (int r = 0; r < 8; r++) {
        if (queue_enqueue(&schedulers[target_core].from_queues[from_idx], actor, msg)) {
            sched_wake(&schedulers[target_core]);
            AETHER_STAT_INC(queue_sends);
            return;
        }
        AETHER_PAUSE();
    }

    // Queue still full after bounded retry. This is a scheduler thread (the
    // non-scheduler producers returned above): defer to overflow and return —
    // never block the loop.
    overflow_append(target_core, actor, msg);
    AETHER_STAT_INC(queue_sends);
}

/* Messages to an actor of another core, enqueued in one batch. */
#define SEND_MANY_CHUNK 256

void scheduler_send_many(ActorBase* actor, const Message* msgs, int n) {
    const int me = current_core_id;
    // Not a core's thread, an actor with a thread of its own, or no actor:
    // each message by the path an unbuffered send of it takes (the shared
    // channel under its lock, refused while the scheduler is stopped, the
    // inbox, the drop). So with a mailbox bound (#2974): each message takes
    // its own place, or is refused, as it goes.
    if (!actor || me < 0 || me >= num_cores || actor->auto_process ||
        atomic_load_explicit(&actor->mailbox_bound, memory_order_relaxed)) {
        for (int i = 0; i < n; i++) scheduler_send_remote(actor, msgs[i], me);
        return;
    }
    int i = 0;
    while (i < n) {
        // Dead, or this core's own: one message as aether_send_message
        // sends it (a local send goes behind what is queued for the actor
        // and its overflow, #3093, #3241, and may step it inline).
        if (atomic_load_explicit(&actor->dead, memory_order_acquire) ||
            atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == me) {
            if (atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == me)
                scheduler_send_local(actor, msgs[i]);
            else
                scheduler_send_remote(actor, msgs[i], me);
            i++;
            continue;
        }
        // Another core's: counted in flight before its core is read again
        // (#3090), as send_through_core does, then one batch through this
        // core's channel to it. Behind this thread's deferred sends to that
        // core, or the batch would pass them (#3093): with any, one message
        // goes the way send_through_core takes it.
        int k = n - i < SEND_MANY_CHUNK ? n - i : SEND_MANY_CHUNK;
        inflight_enter(actor, k);
        int tc = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
        if (tc == me || tc < 0 || tc >= num_cores || tls_overflow[tc].count > 0 ||
            atomic_load_explicit(&actor->dead, memory_order_acquire) != 0) {
            inflight_leave(actor, k);
            scheduler_send_remote(actor, msgs[i], me);
            i++;
            continue;
        }
        void* actors[SEND_MANY_CHUNK];
        for (int j = 0; j < k; j++) actors[j] = actor;
        // Counted before the receiver can take them, as a single send counts
        // (count_pending_messages relies on it).
        count_sent((uint64_t)k);
        int enq = queue_enqueue_batch(&schedulers[tc].from_queues[me], actors,
                                      (Message*)(msgs + i), k);
        // What the queue had no room for waits in this thread's deferred
        // sends, in order and still counted, as a single send's would.
        for (int j = enq; j < k; j++) overflow_append(tc, actor, msgs[i + j]);
        if (enq > 0) {
            sched_wake(&schedulers[tc]);
        }
        // The gather a single send asks for (#3090). An ask is never batched
        // (#3196: its target is not gathered).
        atomic_store_explicit(&actor->migrate_to, me, memory_order_relaxed);
        i += k;
    }
}

// ==============================================================================
// BATCH SEND OPTIMIZATION (for main thread fan-out patterns)
// ==============================================================================
// Reduces atomic operations from N to num_cores by grouping messages by target
// core and using queue_enqueue_batch for bulk insertion.
//
// Performance: For fork-join with 8 workers and 1M messages:
//   - Without batching: ~1M atomic ops (one per message)
//   - With batching: ~4K atomic ops (256 messages per batch, 8 cores)

#define BATCH_SEND_SIZE 256

typedef struct {
    ActorBase* actors[BATCH_SEND_SIZE];
    Message messages[BATCH_SEND_SIZE];
    int count;
    int by_core[MAX_CORES];  // Count per target core for efficient sorting
} BatchSendBuffer;

static AETHER_TLS BatchSendBuffer* g_batch_buffer = NULL;

/* #3117: a send from this thread that does not join its pending batch, or a
 * wait for quiescence, sends the batch first: the batch's messages are older,
 * and messages nobody has sent are not pending to the wait. */
static inline void batch_flush_pending(void) {
    if (unlikely(g_batch_buffer && g_batch_buffer->count > 0)) scheduler_send_batch_flush();
}

/* Opening a batch never drops what one holds (#3117): only a flush empties
 * the buffer. An inner loop's start emptied it and lost the outer loop's
 * messages. */
void scheduler_send_batch_start(void) {
    if (!g_batch_buffer) {
        g_batch_buffer = calloc(1, sizeof(BatchSendBuffer));
        if (!g_batch_buffer) return;  // OOM: batching stays off, callers fall back
    }
}

void scheduler_send_batch_add(ActorBase* actor, Message msg) {
    // A bounded actor's message takes its place as it joins the batch, or
    // is refused (#2974); every way on from here routes it as admitted.
    if (unlikely(actor && atomic_load_explicit(&actor->mailbox_bound, memory_order_relaxed)) &&
        !bound_admits(actor, &msg))
        return;
    // Drop messages to dead actors (see scheduler_send_local).
    if (unlikely(!actor || atomic_load_explicit(&actor->dead, memory_order_acquire))) {
        send_to_dead_actor(actor, &msg);
        return;
    }
    // An actor with its own thread: straight to its inbox, as from
    // scheduler_send_remote, never stepped inline (#2598). And nothing is
    // batched on a scheduler thread (#3117): its sends keep its own core's
    // channel and overflow order.
    if (unlikely(actor->auto_process) || current_core_id >= 0) {
        send_remote_admitted(actor, msg, current_core_id);
        return;
    }
    // BATCH PATH: Multi-actor fan-out optimization
    if (!g_batch_buffer) {
        scheduler_send_batch_start();
        if (!g_batch_buffer) {
            // No buffer: the send goes out on its own, as an unbatched one.
            send_remote_admitted(actor, msg, current_core_id);
            return;
        }
    }

    // If buffer is full, flush first
    if (g_batch_buffer->count >= BATCH_SEND_SIZE) {
        scheduler_send_batch_flush();
    }

    int idx = g_batch_buffer->count++;
    g_batch_buffer->actors[idx] = actor;
    g_batch_buffer->messages[idx] = msg;

    // Track per-core counts for O(1) sorting later
    int target_core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
    if (target_core >= 0 && target_core < num_cores) {
        g_batch_buffer->by_core[target_core]++;
    }
}

void scheduler_send_batch_flush(void) {
    if (!g_batch_buffer || g_batch_buffer->count == 0) return;

    int count = g_batch_buffer->count;
    if (!foreign_send_enter()) {
        for (int i = 0; i < count; i++)
            send_while_stopped(g_batch_buffer->actors[i], &g_batch_buffer->messages[i]);
        g_batch_buffer->count = 0;
        memset(g_batch_buffer->by_core, 0, sizeof(g_batch_buffer->by_core));
        return;
    }

    // === PHASE 1: Snapshot target cores and compute per-core counts ===
    // Read assigned_core ONCE per message and store it.  The by_core[] counts
    // recorded in batch_add can be stale if an actor migrated between add and
    // flush — recomputing from a single consistent snapshot prevents the
    // sorted_actors[] overflow that would result from a count/core mismatch.
    int targets[BATCH_SEND_SIZE];
    int by_core[MAX_CORES];
    memset(by_core, 0, sizeof(by_core));

    // Every message is counted in flight before its actor's core is read
    // (#3090, #3093), as in send_through_core. batch_add sent an actor with
    // its own thread elsewhere, so each one here goes through a queue. One
    // count per actor in the batch, not per message: a fan-out's batch holds
    // a few actors many times each, and each count is a write to a line its
    // actor's core is using.
    {
        ActorBase* seen[32];
        int seen_n[32];
        int nseen = 0;
        for (int i = 0; i < count; i++) {
            ActorBase* a = g_batch_buffer->actors[i];
            int k = 0;
            while (k < nseen && seen[k] != a) k++;
            if (k < nseen) {
                seen_n[k]++;
            } else if (nseen < 32) {
                seen[nseen] = a;
                seen_n[nseen++] = 1;
            } else {
                inflight_enter(a, 1);
            }
        }
        for (int k = 0; k < nseen; k++) inflight_enter(seen[k], seen_n[k]);
    }

    for (int i = 0; i < count; i++) {
        // Its target's death read again now that it is counted (#2974), as
        // in send_through_core, when a restart claim is live: with none, a
        // later claim sees this message in flight and waits for it, and a
        // dead target drops it where it arrives. The death is on a line its
        // core writes at every step, read here a whole batch after the add
        // read it, so reading it for every message cost fork_join 2%. Not
        // counted as sent yet, so no credit.
        if (unlikely((atomic_load_explicit(&aether_caps_slow, memory_order_seq_cst) &
                      AETHER_CAPS_SPAWN_MASK) != 0) &&
            atomic_load_explicit(&g_batch_buffer->actors[i]->dead, memory_order_acquire) != 0) {
            send_to_dead_actor(g_batch_buffer->actors[i], &g_batch_buffer->messages[i]);
            inflight_leave(g_batch_buffer->actors[i], 1);   // after the drop, which reads it
            targets[i] = -1;
            continue;
        }
        int tc = atomic_load_explicit(&g_batch_buffer->actors[i]->assigned_core, memory_order_acquire);
        if (tc < 0 || tc >= num_cores) tc = g_batch_buffer->actors[i]->id % num_cores;
        targets[i] = tc;
        by_core[tc]++;
    }

    // === PHASE 2: Compute offsets for radix sort by core ===
    int offsets[MAX_CORES] = {0};
    int offset = 0;
    for (int c = 0; c < num_cores; c++) {
        offsets[c] = offset;
        offset += by_core[c];
    }

    // === PHASE 3: Sort messages into per-core buckets ===
    void* sorted_actors[BATCH_SEND_SIZE];
    Message sorted_msgs[BATCH_SEND_SIZE];
    int positions[MAX_CORES];
    memcpy(positions, offsets, num_cores * sizeof(int));

    for (int i = 0; i < count; i++) {
        if (targets[i] < 0) continue;   // dropped above
        int pos = positions[targets[i]]++;
        sorted_actors[pos] = g_batch_buffer->actors[i];
        sorted_msgs[pos] = g_batch_buffer->messages[i];
    }
    // Copied out: the buffer is empty before the overflow fallback below
    // sends, which flushes a pending batch first.
    g_batch_buffer->count = 0;
    memset(g_batch_buffer->by_core, 0, sizeof(g_batch_buffer->by_core));

    // === PHASE 4: Batch enqueue to each core (ONE atomic per core!) ===
    for (int c = 0; c < num_cores; c++) {
        int start = offsets[c];
        int cnt = by_core[c];
        if (cnt == 0) continue;

        // Counted before the core can take them, as every send is
        // (count_pending_messages relies on it, #3464): counted after, a
        // core that handled the batch while this thread waited on another
        // core's lock made the handled count pass the sent count.
        atomic_fetch_add_explicit(&main_thread_sent, (uint64_t)cnt, memory_order_relaxed);

        // Batch send is always called from main thread (current_core_id = -1),
        // so use the non-scheduler SPSC channel (from_queues[MAX_CORES]) —
        // under its lock, since a foreign thread may be writing it too
        // (#2083).
        pthread_mutex_lock(&schedulers[c].foreign_lock);
        int enqueued = queue_enqueue_batch(
            &schedulers[c].from_queues[MAX_CORES],
            &sorted_actors[start],
            &sorted_msgs[start],
            cnt
        );
        pthread_mutex_unlock(&schedulers[c].foreign_lock);

        // Fallback for overflow (rare) - scheduler_send_remote handles its own counting,
        // the message in flight included, so this batch's counts are given back first.
        for (int j = enqueued; j < cnt; j++) {
            inflight_leave((ActorBase*)sorted_actors[start + j], 1);
            atomic_fetch_sub_explicit(&main_thread_sent, 1, memory_order_relaxed);
            send_remote_admitted(sorted_actors[start + j], sorted_msgs[start + j], -1);
        }

        // A core that went idle and parked between two batches is told, as
        // every other enqueue tells it (#3087). It was not: it slept on the
        // batch until its timed wait ran out, so a fan-out from main (a
        // fork-join's workers) stalled for a park timeout at every gap longer
        // than the idle spin, and on Windows for at least a 15.6 ms tick.
        if (enqueued > 0) sched_wake(&schedulers[c]);
    }

    foreign_send_leave();

    // Reset buffer
    g_batch_buffer->count = 0;
    memset(g_batch_buffer->by_core, 0, sizeof(g_batch_buffer->by_core));
}

// Spawn actor with NUMA-aware allocation.  actor_size must be >= sizeof(ActorBase)
// and cover the full derived-actor struct (e.g. sizeof(PingActor)).
ActorBase* scheduler_spawn_actor(int preferred_core, void (*step)(void*), size_t actor_size) {
    scheduler_init_on_demand();
    if (preferred_core < 0 || preferred_core >= num_cores) {
        // Spawn on caller's core so parent→child messaging stays local.
        // Main-thread spawns have no parent core: keep -1 so registration
        // balances the pool instead of pinning every worker to core 0.
        preferred_core = current_core_id;
    }
    if (actor_size < sizeof(ActorBase)) actor_size = sizeof(ActorBase);

    // A restart in place (#2974): the factory's spawn of the child's type
    // takes the child's own block, already registered on its core.
    ActorBase* reborn = aether_rebirth_take(step, actor_size);
    if (reborn) return reborn;

    // A released actor's block of this size first (#2517), then the allocator.
    ActorBase* actor = tombstone_take(actor_size);
    if (!actor) {
        int numa_node = aether_numa_node_of_cpu(preferred_core >= 0 ? preferred_core : 0);
        // Generated actor structs are aligned(64), which plain malloc does not
        // honour (#2485). The block goes back through the matching
        // aether_numa_free_aligned once the tables are freed.
        actor = aether_numa_alloc_aligned(actor_size, AETHER_ACTOR_ALIGN, numa_node);
        if (!actor) return NULL;
        AETHER_STAT_INC(actors_malloced);
    }
    mailbox_init(&actor->mailbox);

    actor->alloc_size = actor_size;
    actor->destroy_state = NULL;   // the generated spawn sets it (#2528)
    actor->release_message = NULL; // and this (#3142)
    actor->step_payload = NULL;
    /* Published for scheduler_send_ref, which may read a reused block's id
     * from another thread (#2527). */
    __atomic_store_n(&actor->id, atomic_fetch_add(&next_actor_id, 1), __ATOMIC_RELEASE);
    actor->step = step;
    atomic_init(&actor->active, 0);  // inactive until first message send
    actor->thread = 0;
    actor->auto_process = 0;
    actor->spsc_queue = NULL;  // filled by nothing since #3292
    atomic_init(&actor->inbox, NULL);
    atomic_init(&actor->thread_parked, 0);
    atomic_init(&actor->inflight, 0);
    atomic_init(&actor->death_watch, NULL);         // #2974
    atomic_init(&actor->exit_flags, 0);
    atomic_init(&actor->incarnation, 0);
    atomic_init(&actor->mailbox_bound, 0);      // the generated spawn sets a bound (#2974)
    atomic_init(&actor->mailbox_pending, 0);
    atomic_init(&actor->full_drops, 0);
    atomic_init(&actor->assigned_core, preferred_core);
    atomic_init(&actor->migrate_to, -1);
    atomic_init(&actor->reply_slot, NULL);
    atomic_init(&actor->ready_queued, 0);        // #3540: a reused block has no entry left
    atomic_init(&actor->timer_slot, 0);   // (tombstone_take)
    atomic_flag_clear_explicit(&actor->step_lock, memory_order_relaxed);
    actor->timeout_ns = 0;
    actor->last_activity_ns = 0;
    atomic_init(&actor->dead, 0);

    aether_on_actor_spawn();

    actor->scheduler_owned = 1;
    register_actor(actor, preferred_core);

    return actor;
}

// The rest of a release, once the actor is marked and nothing of its own
// runs it any more: out of its table, off the count, retired.
static void actor_end(ActorBase* actor) {
    // Out of its table only: its entries in the ready lists and the heap
    // go when its core takes them, which the reclaim of its block waits for
    // (a release never waits here: a step can release its own actor).
    (void)actor_table_leave(actor);

    // Track actor count for inline mode auto-detection
    aether_on_actor_terminate();

    // Freed once no reader can hold it (see the reclamation notes), which is
    // never before this call returns: a step can release its own actor.
    actor_retire(actor);
}

void scheduler_release_actor(ActorBase* actor) {
    if (!actor) return;

    // Read before the mark: once it is set, the actor's own thread may end
    // the actor at any moment, and nothing below touches it after that.
    int own_thread = actor->auto_process;
    int park_core = own_thread ? actor_park_core(actor) : -1;

    // Its watchers are told now (#2974), before the mark: once it is set the
    // actor may be ended at any moment, its watch list with it. A release is
    // a normal end unless something ended the actor first; one told already
    // (a death finalized) is told nothing more.
    if (!(atomic_load_explicit(&actor->dead, memory_order_acquire) & AETHER_ACTOR_RELEASED)) {
        aether_actor_claim_reason(actor, "normal");
        aether_watch_notify(actor, aether_actor_death_reason(actor));
    }

    // A message that still reaches the actor is dropped, not delivered.
    int was = atomic_fetch_or_explicit(&actor->dead, AETHER_ACTOR_RELEASED, memory_order_acq_rel);
    if (was & AETHER_ACTOR_RELEASED) return;   // released twice: ended once

    // An actor with its own thread (#2517): that thread holds the actor for
    // as long as it runs, outside the epochs, which only cover pointers found
    // through a table, and this never waits for it (its step may be waiting
    // on the releasing core). The thread leaves its loop at the mark just
    // set and ends the actor itself (scheduler_actor_thread_exit), unless it
    // has left already, in which case that is done here. The two bits make
    // the second of the two the one that ends it.
    // If the thread is asleep in its idle park (#2592), it is woken to see
    // the mark now rather than at the end of its timed wait.
    if (own_thread && !(was & AETHER_ACTOR_THREAD_GONE)) {
        actor_park_broadcast(park_core);
        return;
    }
    actor_end(actor);
}

void scheduler_actor_thread_exit(ActorBase* actor) {
    int was = atomic_fetch_or_explicit(&actor->dead, AETHER_ACTOR_THREAD_GONE, memory_order_acq_rel);
    // Ended after the thread's last look at its bits (#2974): a kill that
    // came then finds the thread gone, or this finds the kill; whichever
    // is second finalizes, once.
    if ((was & AETHER_ACTOR_ENDED) && !(was & AETHER_ACTOR_RELEASED)) actor_finalize(actor);
    if (was & AETHER_ACTOR_RELEASED) actor_end(actor);
}


// ============================================================================
// Ask/Reply support
// ============================================================================

// The ask's reply slot rides in its Message._reply_slot (aether_ask_message).
// The receiver's step function copies Message._reply_slot into
// g_current_reply_slot after mailbox_receive, so scheduler_reply can find it.
AETHER_TLS void* g_current_reply_slot = NULL;

/* The reply slots (#3533). Every ask used to calloc one, build its mutex and
 * condition variable, and destroy and free all of it on the way out. They
 * are made once now and lent: an ask takes one whose count is 0 (nobody
 * holds it) by setting the count to 2, the asker and the actor side, and
 * the last of the two to let go puts it back to 0. The asking thread's last
 * slot is tried first, so a thread that asks again and again uses one slot
 * and touches no shared word; a slot a timed out ask left with the actor is
 * still held, and the next ask takes another until the reply comes and lets
 * it go. They live for the process, on g_reply_slots, as the reclaim
 * readers do: a thread can end without anything telling the runtime, and a
 * slot on the list is still reachable for whoever asks next. */
static _Atomic(ActorReplySlot*) g_reply_slots = NULL;
static AETHER_TLS ActorReplySlot* t_reply_slot = NULL;

static int reply_slot_take(ActorReplySlot* slot) {
    int none = 0;
    if (!atomic_compare_exchange_strong_explicit(&slot->refcount, &none, 2,
                                                 memory_order_acquire, memory_order_relaxed))
        return 0;
    /* Free means both sides are done with the last ask's fields. */
    slot->reply_data = NULL;
    slot->reply_size = 0;
    slot->reply_ready = 0;
    slot->timed_out = 0;
    slot->failed = 0;
    slot->deadline_ns = 0;
    t_reply_slot = slot;
    return 1;
}

/* A slot for an ask, its count 2; NULL when none is free and no memory is
 * left for another. */
static ActorReplySlot* reply_slot_lend(void) {
    ActorReplySlot* mine = t_reply_slot;
    if (mine && reply_slot_take(mine)) return mine;
    for (ActorReplySlot* s = atomic_load_explicit(&g_reply_slots, memory_order_acquire); s; s = s->all_next)
        if (s != mine && reply_slot_take(s)) return s;
    ActorReplySlot* slot = (ActorReplySlot*)calloc(1, sizeof(ActorReplySlot));
    if (!slot) return NULL;
    pthread_mutex_init(&slot->mutex, NULL);
    pthread_cond_init(&slot->cond, NULL);
    atomic_init(&slot->refcount, 2);  // asker + actor handler
    ActorReplySlot* head = atomic_load_explicit(&g_reply_slots, memory_order_relaxed);
    do {
        slot->all_next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_reply_slots, &head, slot,
                                                    memory_order_release, memory_order_relaxed));
    t_reply_slot = slot;
    return slot;
}

/* One side lets go. At zero the slot is free for the next ask: nothing of
 * it may be touched after this by the side that let go. */
static void reply_slot_decref(ActorReplySlot* slot) {
    atomic_fetch_sub_explicit(&slot->refcount, 1, memory_order_acq_rel);
}

/* An ask that can get no reply (#3151): its message was dropped (a dead or
 * released target, a mailbox drained at death) or the actor died handling
 * it. The asker is woken with the reason instead of waiting out its timeout,
 * and the reference held for the reply goes. */
static void reply_slot_fail(ActorReplySlot* slot, int why) {
    pthread_mutex_lock(&slot->mutex);
    if (!slot->reply_ready && !slot->failed) {
        slot->failed = why;
        pthread_cond_signal(&slot->cond);
    }
    pthread_mutex_unlock(&slot->mutex);
    reply_slot_decref(slot);
}

/* ---------------------------------------------------------------------------
 * Relief of a core whose thread waits in an ask (#3197)
 *
 * An ask made in a handler blocks its core's scheduler thread until the
 * reply, and nothing else runs the core meanwhile: its queued messages are
 * not delivered, and its other actors are not stepped. An actor elsewhere
 * that, while answering, asks one of them waited for the reply too, to the
 * timeout, and every other actor of the core waited with the asker.
 *
 * So the waiting thread, once its wait has lasted ASK_RELIEF_NS and then
 * every ASK_RELIEF_NS (ASK_RELIEF_BUSY_NS after a relief that did
 * something), does the part of its loop that steps nothing:
 *
 * - its deferred sends go out, its own core's through its own queue;
 * - its queued messages go where the loop would put them, in order: into
 *   its actors' mailboxes, to an actor's own thread, or on to the core an
 *   actor moved to. A sender's queue is taken up to the first message that
 *   cannot go now (its actor's mailbox is full, or the batch the loop was
 *   delivering when the wait began still holds a message for it), which
 *   the loop delivers once it runs again;
 * - its actors with work (a message, a receive timeout due) and nothing in
 *   flight are moved by move_actor, as every move between cores is, to a
 *   core that will run them: the core waiting on that very actor, whose
 *   thread steps it, else a parked core, else the least loaded core that
 *   is not waiting in an ask. move_actor moves nothing while a message sent
 *   from this core is still on its way (#3240). An actor stepping on this
 *   thread (the asker, and any below it) holds its step lock and stays.
 *
 * A message for an actor whose mailbox is full goes into the actor's
 * overflow, which starts there (#3241, see the overflow notes at the top):
 * the actor's from then on, it no longer holds the actor on this core, and
 * an actor with more messages queued than its mailbox holds moves with all
 * of them, in order. Left in the queue, as it was, it kept the actor here
 * until the ask ended, since moved, its senders' next messages would leave
 * through other queues and could overtake it.
 *
 * Nothing is stepped here. An actor stepped on this thread would run inside
 * the asker's handler, and one that asked the asker in turn could never be
 * answered; moved to another core it runs on that core's thread, and an ask
 * of the asker waits there until the asker's handler is done, as it would
 * have.
 * ------------------------------------------------------------------------- */
#define ASK_RELIEF_NS 1000000ULL          /* 1 ms */
#define ASK_RELIEF_BUSY_NS 50000ULL       /* 50 us, after a relief that did something */
#define ASK_RELIEF_IDLE_MAX_NS 8000000ULL /* 8 ms, the gap reliefs that find nothing grow to */
#define ASK_RELIEF_DELIVER_MAX 4096       /* messages placed per relief */
#define ASK_RELIEF_SCAN_MAX 4096          /* actors looked at per relief */

/* Whether the batch the loop was delivering still holds a message for
 * `actor`: those are older than anything still queued. */
static int relief_batch_holds(const Scheduler* sched, const void* actor) {
    for (int k = sched->coalesce_buffer.next; k < sched->coalesce_buffer.count; k++)
        if (sched->coalesce_buffer.actors[k] == actor) return 1;
    return 0;
}

/* A message for `actor`, this core's, whose mailbox is full and whose
 * overflow is empty: the overflow starts with it (#3241). 0 only when the
 * overflow cannot be made or grown; the message then stays where it was. */
static int actor_overflow_start(ActorBase* actor, Message msg) {
    ActorInbox* ib = actor_inbox_of(actor);
    return ib && actor_inbox_push(ib, msg);
}

/* The batch's messages for actors of this core that are not stepping, into
 * their mailboxes in order (behind their overflows, or starting them, #3241),
 * taken out of the batch: they are the oldest each has, and an actor asked
 * while its message waited there was stepped by nothing until the ask timed
 * out. The loop skips a taken entry. An actor stepping (its step lock held:
 * the asker, one below it, the one make_room is making room in) keeps its
 * entries, and so does one whose message finds no place, from that entry
 * on, so none of its messages passes another. */
static int relief_claim_batch(Scheduler* sched) {
    ActorBase* kept[64];
    int nkept = 0, taken = 0;
    for (int k = sched->coalesce_buffer.next; k < sched->coalesce_buffer.count; k++) {
        ActorBase* actor = (ActorBase*)sched->coalesce_buffer.actors[k];
        if (!actor) continue;
        int skip = 0;
        for (int j = 0; j < nkept && !skip; j++) skip = kept[j] == actor;
        if (skip) continue;
        int mine = !actor->auto_process && actor->step &&
                   !atomic_load_explicit(&actor->dead, memory_order_acquire) &&
                   atomic_load_explicit(&actor->assigned_core, memory_order_acquire) == sched->core_id;
        int placed = 0;
        if (mine && !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
            Message msg = sched->coalesce_buffer.messages[k];
            placed = deliver_owned(actor, msg) || actor_overflow_start(actor, msg);
            atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
        }
        if (placed) {
            sched->coalesce_buffer.actors[k] = NULL;
            inflight_leave(actor, 1);
            atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
            ready_push(actor);   // stepped by the loop, or moved by the relief (#3540)
            taken++;
        } else if (nkept < 64) {
            kept[nkept++] = actor;
        } else {
            break;   // more actors refused than tracked: the rest waits for the loop
        }
    }
    return taken;
}

/* The queue's next message, where the loop would put it, or 0 when it has to
 * wait for the loop. */
static int relief_place(Scheduler* sched, ActorBase* actor, Message* msg) {
    if (atomic_load_explicit(&actor->dead, memory_order_acquire)) {
        int counted = !actor->auto_process;
        scheduler_drop_dead_message(sched, actor, msg);
        if (counted) inflight_leave(actor, 1);   // after the drop, which reads it
        return 1;
    }
    if (relief_batch_holds(sched, actor)) return 0;
    // An actor with a thread of its own takes it in its inbox, from any thread.
    if (actor->auto_process) {
        scheduler_actor_thread_deliver(actor, *msg);
        return 1;
    }
    int ac = atomic_load_explicit(&actor->assigned_core, memory_order_acquire);
    if (ac != sched->core_id) {
        // Another core's now: forwarded, still counted, as the loop does.
        if (ac < 0 || ac >= num_cores ||
            !queue_enqueue(&schedulers[ac].from_queues[sched->core_id], actor, *msg))
            return 0;
        sched_wake(&schedulers[ac]);
        return 1;
    }
    // Into the mailbox, or behind the overflow, and a full mailbox starts
    // one (#3241). One with no handler takes nothing more: the loop drops
    // what is queued for it.
    if (!deliver_owned(actor, *msg) && !(actor->step && actor_overflow_start(actor, *msg))) return 0;
    inflight_leave(actor, 1);
    atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    ready_push(actor);   // stepped by the loop, or moved by the relief (#3540)
    return 1;
}

static int relief_deliver(Scheduler* sched) {
    int claimed = relief_claim_batch(sched);
    int budget = ASK_RELIEF_DELIVER_MAX;
    for (int q = 0; q <= MAX_CORES && budget > 0; q++) {
        LockFreeQueue* fq = &sched->from_queues[q];
        QueueItem* it;
        while (budget > 0 && (it = queue_peek(fq)) != NULL &&
               relief_place(sched, (ActorBase*)it->actor, &it->msg)) {
            queue_pop(fq);
            budget--;
        }
    }
    return claimed + ASK_RELIEF_DELIVER_MAX - budget;
}

/* The core to take `actor` to: the one waiting on it, else one parked for
 * want of work, else the one with the fewest messages queued for it, not
 * waiting in an ask (a core in a long step runs nothing either, and its
 * queue shows it); -1 for none. */
static int relief_destination(const Scheduler* sched, ActorBase* actor) {
    int best = -1, best_work = 0, best_parked = 0;
    for (int k = 0; k < num_cores; k++) {
        if (k == sched->core_id) continue;
        Scheduler* s = &schedulers[k];
        if (atomic_load_explicit(&s->asking, memory_order_acquire) > 0) {
            if (atomic_load_explicit(&s->ask_target, memory_order_acquire) == actor) return k;
            continue;
        }
        int parked = atomic_load_explicit(&s->parked, memory_order_relaxed) != 0;
        int work = 0;
        for (int q = 0; q <= MAX_CORES; q++) work += queue_size(&s->from_queues[q]);
        if (best < 0 || parked > best_parked || (parked == best_parked && work < best_work)) {
            best = k;
            best_work = work;
            best_parked = parked;
        }
    }
    return best;
}

static int relief_evacuate(Scheduler* sched) {
    int n = 0;
    AetherActorTable* table = actor_table_snapshot(sched, &n);
    if (n <= 0 || !core_outbound_idle(sched->core_id)) return 0;
    int moved = 0;
    ActorBase* waited = atomic_load_explicit(&sched->ask_target, memory_order_relaxed);
    uint64_t now = aether_now_ns();
    int start = sched->relief_cursor < n ? sched->relief_cursor : 0;
    int looked = n < ASK_RELIEF_SCAN_MAX ? n : ASK_RELIEF_SCAN_MAX;
    for (int k = 0; k < looked; k++) {
        int i = (start + k) % n;
        ActorBase* actor = actor_table_read(table, i);
        // The actor this thread waits on is stepped by its wait.
        if (!actor || actor == waited || actor->auto_process || !actor->step ||
            atomic_load_explicit(&actor->inflight, memory_order_acquire) != 0)
            continue;
        uint64_t armed = actor->last_activity_ns;
        int work = atomic_load_explicit(&actor->mailbox.count, memory_order_acquire) > 0 ||
                   (actor->timeout_ns > 0 && armed > 0 && now > armed &&
                    now - armed >= actor->timeout_ns);
        if (!work) continue;
        int dst = relief_destination(sched, actor);
        if (dst >= 0 && move_actor(sched, actor, dst, i, 1)) moved++;
    }
    sched->relief_cursor = (start + looked) % n;
    return moved;
}

/* One relief. Returns whether it did anything (placed a message, moved an
 * actor), when the next comes ASK_RELIEF_BUSY_NS later. */
static int ask_relieve(Scheduler* sched) {
    control_run(sched);   // deaths to finalize (#2974); one stepping here waits for its step
    overflow_flush_ex(sched->core_id, 0);
    int placed = relief_deliver(sched);
    int moved = relief_evacuate(sched);
    // Their entries in this core's ready list go with them (#3540).
    if (moved > 0) ready_forward_moved(sched);
    return placed + moved > 0;
}


/* How deep the asks this thread steps targets for are nested (#3220): each
 * step is a frame on this thread's stack. */
static AETHER_TLS int ask_help_depth = 0;
#define MAX_ASK_HELP_DEPTH 32

void* scheduler_ask_message_status(ActorBase* target, void* msg_data, size_t msg_size,
                                   int timeout_ms, int* status) {
    int why_dummy;
    if (!status) status = &why_dummy;
    *status = AETHER_ASK_DEAD;
    if (!target || !msg_data) return NULL;
    /* A dead actor answers nothing: no message, no wait (#3151). */
    if (atomic_load_explicit(&target->dead, memory_order_acquire)) return NULL;

    ActorReplySlot* slot = reply_slot_lend();
    if (!slot) {
        /* The script gets the error; a library call ends at its next safe
         * point with AETHER_OUT_OF_MEMORY (#2956). */
        aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
        *status = AETHER_ASK_NO_MEMORY;
        return NULL;
    }

    /* Its message, with the slot in it (#3395), before anything of this
     * thread's or its core's is marked: a copy that finds no memory fails
     * the ask, as the slot's own allocation does, with nothing to restore
     * (#3387). An unwind from here once left the core marked as asking and
     * the slot set for every later send. */
    Message ask_msg;
    if (!aether_ask_message(target, msg_data, msg_size, slot, &ask_msg)) {
        /* Nobody else has seen it: both references go back at once. */
        atomic_store_explicit(&slot->refcount, 0, memory_order_release);
        aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
        *status = AETHER_ASK_NO_MEMORY;
        return NULL;
    }

    /* An ask blocks this thread until the reply. Made by a handler, this is
     * a core's scheduler thread, which then steps nothing else (#3196,
     * #3220). So the core is marked as asking, and from then until the
     * reply no actor is moved onto it (move_actor checks the mark under the
     * locks a move takes, and the mark is set under this core's own): a
     * send the handler made earlier asked the target's core to gather it
     * here (#3090), which it did as the ask's message reached it. And the
     * target, if it is this core's, is stepped by this thread while it
     * waits (below), as a send steps an idle actor of the core: nothing
     * else will step it. The target and the slot are the core's too, for
     * another core's relief, which brings that actor here; this thread
     * relieves its own core as it waits (#3197). An actor with a thread of
     * its own reads its core's id but is not that core's loop, and marks
     * nothing. */
    Scheduler* asking_core = NULL;
    ActorBase* outer_target = NULL;
    ActorReplySlot* outer_slot = NULL;
    ActorBase* outer_asker = NULL;
    /* The actor whose handler asks, if any: killed while it waits, its ask
     * ends at once (#2974). */
    ActorBase* asker = (ActorBase*)g_current_step_actor;
    int killed = 0;
    /* Only a core's own scheduler thread (#3246): it alone may step for the
     * core, deliver its queues and move its actors. */
    if (t_reclaim_core_thread && current_core_id >= 0 && current_core_id < num_cores) {
        asking_core = &schedulers[current_core_id];
        while (atomic_flag_test_and_set_explicit(&asking_core->actor_lock.lock, memory_order_acquire))
            AETHER_PAUSE();
        atomic_fetch_add_explicit(&asking_core->asking, 1, memory_order_acq_rel);
        outer_target = atomic_load_explicit(&asking_core->ask_target, memory_order_relaxed);
        outer_slot = atomic_load_explicit(&asking_core->ask_slot, memory_order_relaxed);
        atomic_store_explicit(&asking_core->ask_target, target, memory_order_release);
        atomic_store_explicit(&asking_core->ask_slot, slot, memory_order_release);
        outer_asker = atomic_load_explicit(&asking_core->ask_asker, memory_order_relaxed);
        atomic_store_explicit(&asking_core->ask_asker, asker, memory_order_release);
        atomic_flag_clear_explicit(&asking_core->actor_lock.lock, memory_order_release);
    }

    // Each Message carries its own slot, so concurrent asks to one actor
    // each get their own reply.
    aether_send_built(target, ask_msg);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    // A core's wait wakes for its reliefs (#3197), by the monotonic clock.
    uint64_t waited_from = asking_core ? aether_now_ns() : 0;
    uint64_t deadline_ns = waited_from + (uint64_t)timeout_ms * 1000000ULL;
    uint64_t relief_at = waited_from + ASK_RELIEF_NS;
    uint64_t relief_gap = ASK_RELIEF_NS;   // doubles while reliefs find nothing

    pthread_mutex_lock(&slot->mutex);
    while (!slot->reply_ready && !slot->failed) {
        if (asker && (atomic_load_explicit(&asker->dead, memory_order_acquire) & AETHER_ACTOR_KILLED)) {
            killed = 1;
            break;
        }
        /* The target is this core's, with a message still in the batch the
         * loop was delivering when the ask began: the ask's own message
         * waits behind it, and nothing would place either (#3197). The
         * relief's delivery takes them into its mailbox, in order, now. */
        if (asking_core && !target->auto_process &&
            atomic_load_explicit(&target->assigned_core, memory_order_acquire) == current_core_id &&
            relief_batch_holds(asking_core, target)) {
            pthread_mutex_unlock(&slot->mutex);
            int got = relief_deliver(asking_core);
            pthread_mutex_lock(&slot->mutex);
            if (got > 0) continue;
        }
        /* The target is this core's: this thread steps it, a message at a
         * time, until the reply comes or it has nothing left (#3220). An
         * ask it makes in turn does the same for its own target, so a chain
         * of asks among this core's actors runs here, bounded in depth as
         * inline steps are. A target that is stepping (the asking actor
         * itself, higher up this thread) is waited for, to the timeout. One
         * with a thread of its own is that thread's to step, without the
         * step lock, whatever core id it reads (#2598). */
        if (asking_core && ask_help_depth < MAX_ASK_HELP_DEPTH && !target->auto_process &&
            atomic_load_explicit(&target->assigned_core, memory_order_acquire) == current_core_id &&
            atomic_load_explicit(&target->mailbox.count, memory_order_acquire) > 0 &&
            !atomic_load_explicit(&target->dead, memory_order_acquire) &&
            !atomic_flag_test_and_set_explicit(&target->step_lock, memory_order_acquire)) {
            pthread_mutex_unlock(&slot->mutex);   /* the reply takes it */
            ask_help_depth++;
            int took = aether_step_safe(target);
            ask_help_depth--;
            count_processed((uint64_t)took);
            if (atomic_load_explicit(&target->mailbox.count, memory_order_relaxed) == 0)
                atomic_store_explicit(&target->active, 0, memory_order_relaxed);
            atomic_flag_clear_explicit(&target->step_lock, memory_order_release);
            pthread_mutex_lock(&slot->mutex);
            continue;
        }
        if (asking_core) {
            /* A core's thread: relieve the core on schedule, and wait no
             * longer than the next relief in between. */
            uint64_t now = aether_now_ns();
            if (now >= deadline_ns) break;
            if (now >= relief_at) {
                pthread_mutex_unlock(&slot->mutex);
                int busy = ask_relieve(asking_core);
                pthread_mutex_lock(&slot->mutex);
                // Soon after one that did something; further apart while
                // they find nothing, up to ASK_RELIEF_IDLE_MAX_NS.
                uint64_t next = busy ? ASK_RELIEF_BUSY_NS : relief_gap;
                relief_gap = busy ? ASK_RELIEF_NS
                                  : (relief_gap * 2 < ASK_RELIEF_IDLE_MAX_NS ? relief_gap * 2
                                                                             : ASK_RELIEF_IDLE_MAX_NS);
                relief_at = aether_now_ns() + next;
                continue;
            }
            uint64_t until = relief_at < deadline_ns ? relief_at : deadline_ns;
            struct timespec slice;
            clock_gettime(CLOCK_REALTIME, &slice);
            uint64_t ns = (uint64_t)slice.tv_nsec + (until - now);
            slice.tv_sec += (time_t)(ns / 1000000000ULL);
            slice.tv_nsec = (long)(ns % 1000000000ULL);
            pthread_cond_timedwait(&slot->cond, &slot->mutex, &slice);
            continue;
        }
        if (pthread_cond_timedwait(&slot->cond, &slot->mutex, &ts) == ETIMEDOUT) break;
    }
    void* result = slot->reply_ready ? slot->reply_data : NULL;
    if (slot->reply_ready) {
        *status = AETHER_ASK_REPLIED;
    } else if (slot->failed) {
        *status = slot->failed;
    } else {
        // Given up, at the timeout or killed (#2974): a reply that comes
        // later frees the slot itself.
        slot->timed_out = 1;
        *status = killed ? AETHER_ASK_KILLED : AETHER_ASK_TIMED_OUT;
    }
    pthread_mutex_unlock(&slot->mutex);
    if (asking_core) {
        /* Under the lock a relief's move reads them with: once it is let go
         * the slot may be freed, and no relief may wake it after. */
        while (atomic_flag_test_and_set_explicit(&asking_core->actor_lock.lock, memory_order_acquire))
            AETHER_PAUSE();
        atomic_store_explicit(&asking_core->ask_target, outer_target, memory_order_release);
        atomic_store_explicit(&asking_core->ask_slot, outer_slot, memory_order_release);
        atomic_store_explicit(&asking_core->ask_asker, outer_asker, memory_order_release);
        atomic_fetch_sub_explicit(&asking_core->asking, 1, memory_order_acq_rel);
        atomic_flag_clear_explicit(&asking_core->actor_lock.lock, memory_order_release);
    }

    /* The replier lets go of the slot just after it wakes this thread, from
     * its own thread. Answered or failed, the ask spins a moment for that,
     * so the slot is free for this thread's next ask (#3533), and lets go
     * of it here either way. It does not wait any longer: a replier
     * preempted between its unlock and its decref may not run again for a
     * time slice or more, and waiting it out (#3245 waited until it came,
     * yielding) held the asker for up to 3.5 s on an oversubscribed
     * FreeBSD host, where sched_yield gives the CPU to nothing on another
     * run queue, and failed test_ask_slot_reuse. #3245 waited so the slot
     * was freed before the ask returned; slots are lent, not freed, since
     * #3533, so a slot the replier still holds is simply let go by it, as a
     * timed out ask's is, and the next ask takes another meanwhile. A timed
     * out ask, and one its killed asker gave up, do not spin at all. */
    if (*status != AETHER_ASK_TIMED_OUT && *status != AETHER_ASK_KILLED) {
        for (int spins = 0; spins < 256 &&
             atomic_load_explicit(&slot->refcount, memory_order_acquire) > 1; spins++)
            AETHER_PAUSE();
    }
    reply_slot_decref(slot);
    return result;
}

void* scheduler_ask_message(ActorBase* target, void* msg_data, size_t msg_size, int timeout_ms) {
    return scheduler_ask_message_status(target, msg_data, msg_size, timeout_ms, NULL);
}

void scheduler_reply_owned(ActorBase* self, void* data, size_t data_size,
                           void (*release)(void*)) {
    (void)self;
    ActorReplySlot* slot = (ActorReplySlot*)g_current_reply_slot;
    g_current_reply_slot = NULL;
    if (!slot) {
        if (release && data) release(data);   // nobody asked: nothing takes the fields
        return;
    }

    pthread_mutex_lock(&slot->mutex);
    int delivered = 0;
    if (!slot->timed_out) {
        if (data && data_size > 0) {
            slot->reply_data = malloc(data_size);
            if (slot->reply_data) {
                memcpy(slot->reply_data, data, data_size);
                slot->reply_size = data_size;
                delivered = 1;
            }
        }
        slot->reply_ready = 1;
        pthread_cond_signal(&slot->cond);
    }
    pthread_mutex_unlock(&slot->mutex);
    // The asker gave up (or the copy failed): what the reply owns, the
    // strings copied and closures taken for the asker, has no taker but the
    // replier (#2528). Delivered, the bytes are the asker's to release.
    if (!delivered && release && data) release(data);

    reply_slot_decref(slot);
}

void scheduler_reply(ActorBase* self, void* data, size_t data_size) {
    scheduler_reply_owned(self, data, data_size, NULL);
}

// aether_scheduler_poll: the scheduler threads run every actor from the first
// spawn on (#3112), so a host thread has nothing to drain. See the header.
int aether_scheduler_poll(int max_per_actor) {
    (void)max_per_actor;
    return 0;
}


/* See multicore_scheduler.h: generated code reaches these through calls, never
 * by naming the thread-local, so the TLS model stays inside this archive. */
int  aether_core_id_get(void)            { return current_core_id; }
void aether_core_id_set(int core_id)     { current_core_id = core_id; }
void aether_reply_slot_set(void* slot)   { g_current_reply_slot = slot; }
void aether_step_actor_set(void* actor)  { g_current_step_actor = actor; }
