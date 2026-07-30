// Partitioned State Machine Scheduler - Zero-Sharing Multi-core
// Strategy: Static actor-to-core assignment, no atomics, perfect cache locality

// Platform defines must come first
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <stdio.h>
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
#include "../utils/aether_cpu_detect.h"
#include "../utils/aether_compiler.h"
#include "../config/aether_optimization_config.h"
#include "../aether_numa.h"
#include "../actors/aether_send_buffer.h"
#include "../actors/aether_panic.h"


// Forward declaration to avoid header cycle with aether_send_message.h
extern void aether_send_message(void* actor_ptr, void* message_data, size_t message_size);

// Forward declaration: I/O polling used in scheduler thread loop
static int scheduler_io_poll(Scheduler* sched, int timeout_ms);

// Forward declaration: TLS guard set by aether_send_message_sync while an
// actor's step() is executing synchronously on the main thread.  Used to
// defer main_thread_only=0 in scheduler_spawn_actor and prevent a scheduler
// thread from entering the same step() concurrently.
extern AETHER_TLS ActorBase* g_sync_step_actor;

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

// --------------------------------------------------------------------------
// aether_step_safe — call actor->step() inside a sigsetjmp barrier.
//
// If the handler calls aether_panic() or takes a caught signal, the barrier
// unwinds: we mark the actor dead, fire the death hook, and return normally.
// The scheduler loop keeps running; other actors are unaffected.
//
// Dead actors are skipped on entry: a second attempt to step a crashed
// actor is a no-op.
//
// sigsetjmp(buf, 1) saves the signal mask so siglongjmp from a signal
// handler (Phase 2) restores it. Overhead per call is a handful of
// instructions plus one sigprocmask on systems where glibc implements it
// as a syscall — measurable but small (~30ns on modern x86_64).
//
// We use a function rather than a macro so the 8 step() sites stay
// readable; `inline` lets the compiler devirtualize where it can.
// --------------------------------------------------------------------------
static inline void aether_step_safe(ActorBase* actor) {
    if (!actor || !actor->step) return;
    if (atomic_load_explicit(&actor->dead, memory_order_acquire)) return;

    AetherJmpFrame* f = aether_try_push();
    if (AETHER_SIGSETJMP(f->buf, 1) == 0) {
        g_aether_in_actor_step = 1;
        g_aether_current_actor_id = actor->id;
        actor->step(actor);
        g_aether_in_actor_step = 0;
        g_aether_current_actor_id = -1;
        aether_try_pop();
        return;
    }

    // Unwound via panic or signal.
    const char* reason = f->reason;
    g_aether_in_actor_step = 0;
    g_aether_current_actor_id = -1;
    aether_try_pop();

    atomic_store_explicit(&actor->dead, 1, memory_order_release);
    atomic_store_explicit(&actor->active, 0, memory_order_release);

    // Drain any pending messages so scheduler_wait() sees counters converge.
    // Without this the dead actor's mailbox keeps mailbox.count > 0 forever,
    // scheduler threads skip the actor (dead check), and sent > processed
    // indefinitely — scheduler_wait() would spin or time out.
    int drained = 0;
    Message _discard;
    while (mailbox_receive(&actor->mailbox, &_discard)) {
        drained++;
    }
    if (drained > 0) {
        if (current_core_id >= 0 && current_core_id < num_cores) {
            atomic_fetch_add_explicit(
                &schedulers[current_core_id].messages_processed,
                (uint64_t)drained, memory_order_relaxed);
        } else {
            // Main-thread death: attribute to core 0 so the global sent/processed
            // accounting balances out.
            atomic_fetch_add_explicit(
                &schedulers[0].messages_processed,
                (uint64_t)drained, memory_order_relaxed);
        }
    }

    // Release step_lock so the dead-actor check in future iterations can
    // proceed without contention. (step_lock is a TAS lock; we clear it.)
    atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
    aether_fire_death_hook(actor->id, reason);
}

// Layout assertions to catch struct padding/size mismatches between translation units
#if INTPTR_MAX == INT64_MAX
_Static_assert(sizeof(Message) == 48, "Message size changed — update tests");
#endif
_Static_assert(sizeof(Mailbox) % 8 == 0, "Mailbox not 8-byte aligned");

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

// Track whether scheduler threads have been created.
// scheduler_start() skips thread creation in main-thread mode; if main-thread
// mode is later disabled (e.g. self-send pattern), we need to know whether
// threads still need to be started.
static atomic_int g_threads_started = 0;

// Barrier: scheduler_start() spins here until every thread has finished its
// setup (pin_to_core, send_buffer_init) and entered its main loop.  This
// ensures that by the time scheduler_start() returns, all scheduler threads
// are actively polling from_queues — eliminating the "thread created but not
// yet scheduled" race that causes tests to time out before any messages are
// processed.
static atomic_int g_threads_ready = 0;

AETHER_TLS int current_core_id = -1;
AETHER_TLS void* g_current_step_actor = NULL;

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
                fprintf(stderr, "aether: OOM in overflow_append (target=%d count=%d cap=%d)\n",
                        target, b->count, b->capacity);
                abort();
            }
            b->actors = new_actors;
            Message* new_msgs = realloc(b->msgs, (size_t)nc * sizeof(Message));
            if (unlikely(!new_msgs)) {
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
static void overflow_flush(int from_core) {
    if (!tls_overflow_any) return;
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
        // uniform queue-based path below to avoid amplification.
        if (t == from_core && from_core >= 0 && b->count <= 4096) {
            int start = b->head;
            int orig_count = b->count;
            int limit = orig_count < 128 ? orig_count : 128;
            int rem = 0;  // compacted "keep" entries [start..start+rem)
            for (int i = 0; i < limit; i++) {
                ActorBase* actor = b->actors[start + i];
                Message msg = b->msgs[start + i];
                int ac = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
                if (ac == from_core && !actor->auto_process &&
                    !atomic_load_explicit(&actor->main_thread_only, memory_order_relaxed)) {
                    if (mailbox_send(&actor->mailbox, msg)) {
                        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
                        flushed++;
                        // Immediately process if step_lock available
                        if (actor->step &&
                            !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
                            int p = 0;
                            while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 &&
                                   p < 16 &&
                                   atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == from_core) {
                                aether_step_safe(actor);
                                p++;
                            }
                            atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
                            direct_processed += p;
                        }
                    } else {
                        // Mailbox full — keep in overflow
                        b->actors[start + rem] = actor;
                        b->msgs[start + rem]   = msg;
                        rem++;
                    }
                } else if (ac >= 0 && ac < num_cores) {
                    // Actor migrated away — try to forward
                    if (queue_enqueue(&schedulers[ac].from_queues[from_idx], actor, msg)) {
                        atomic_fetch_add_explicit(&schedulers[ac].work_count, 1, memory_order_relaxed);
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
                atomic_fetch_add_explicit(&schedulers[t].work_count, 1,
                                          memory_order_relaxed);
                flushed++;
            }
            int drained = i - start;
            b->head += drained;
            b->count -= drained;
            if (b->count > 0) any = 1;
        }
    }
    if (flushed > 0) {
        atomic_fetch_sub_explicit(&g_overflow_total, flushed, memory_order_relaxed);
    }
    if (direct_processed > 0 && from_core >= 0) {
        schedulers[from_core].messages_processed += direct_processed;
    }
    tls_overflow_any = any;
}

// Lazy-allocate the SPSC queue for auto_process actors.
// Called on the first spsc_enqueue; regular actors never allocate this.
static inline SPSCQueue* ensure_spsc_queue(ActorBase* actor) {
    if (likely(actor->spsc_queue)) return actor->spsc_queue;
    SPSCQueue* q = calloc(1, sizeof(SPSCQueue));
    if (!q) { fprintf(stderr, "aether: OOM allocating SPSCQueue\n"); abort(); }
    spsc_queue_init(q);
    actor->spsc_queue = q;
    return q;
}

// Pin thread to specific CPU core (NUMA awareness)
// Full support on Linux, macOS, and Windows
static void pin_to_core(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
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
    DWORD_PTR mask = (DWORD_PTR)1 << core_id;
    SetThreadAffinityMask(GetCurrentThread(), mask);
#endif
}

// Check if any from_queue on this scheduler has pending cross-core messages.
// Used to yield early from inner mailbox-drain loops so that external sends
// (e.g. StopAnimation from main) are not starved by a self-scheduling actor.
// Defined here (before scheduler_thread) so the static inline is visible.
static inline int has_pending_cross_core(Scheduler* sched) {
    for (int q = 0; q <= MAX_CORES; q++) {
        if (queue_size(&sched->from_queues[q]) > 0) return 1;
    }
    return 0;
}

// Partitioned scheduler thread with work-stealing fallback for idle cores
void* AETHER_HOT scheduler_thread(void* arg) {
    Scheduler* sched = (Scheduler*)arg;
    current_core_id = sched->core_id;

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

    while (atomic_load_explicit(&sched->running, memory_order_acquire)) {
        int work_done = 0;

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
        if (batch_size > COALESCE_THRESHOLD) batch_size = COALESCE_THRESHOLD;
        
        // TIER 1 ALWAYS ON: Message coalescing (batch dequeue reduces atomics from N to 1)
        // Drain all per-sender SPSC channels round-robin into the coalesce buffer.
        // Each channel has exactly one producer (SPSC), so no locks needed.
        {
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
        }

        // Process coalesced batch with minimal overhead
        for (int i = 0; i < sched->coalesce_buffer.count; i++) {
            ActorBase* actor = (ActorBase*)sched->coalesce_buffer.actors[i];
            Message msg = sched->coalesce_buffer.messages[i];

            // Actor may have been migrated to another core after this message
            // was enqueued.  Forward to the actor's current core rather than
            // delivering here (which would race with the new core's processing).
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
                            forwarded = 1; break;
                        }
                        int cur = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
                        if (cur >= 0 && cur < num_cores) new_core = cur;
                        AETHER_PAUSE();
                    }
                    if (!forwarded) overflow_append(new_core, actor, msg);
                } else {
                    // Actor has an invalid assigned_core — deliver directly to mailbox
                    // to prevent silent message loss.
                    if (!mailbox_send(&actor->mailbox, msg)) {
                        if (!queue_enqueue(&sched->from_queues[sched->core_id], actor, msg)) {
                            overflow_append(sched->core_id, actor, msg);
                        }
                    }
                    atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
                }
                work_done = 1;
                continue;
            }

            // auto_process actors own their mailbox from their thread;
            // deliver via SPSC queue (thread-safe) instead of mailbox.
            if (unlikely(actor->auto_process)) {
                if (!spsc_enqueue(ensure_spsc_queue(actor), msg)) {
                    // SPSC full - re-queue for next iteration via self-channel.
                    // Overflow to TLS buffer if self-channel also full.
                    if (!queue_enqueue(&sched->from_queues[sched->core_id], actor, msg)) {
                        overflow_append(sched->core_id, actor, msg);
                    }
                } else {
                    work_done = 1;
                }
                continue;
            }

            // For actors processed on main thread, just deliver to mailbox (don't process)
            if (unlikely(atomic_load_explicit(&actor->main_thread_only, memory_order_acquire))) {
                if (!mailbox_send(&actor->mailbox, msg)) {
                    if (!queue_enqueue(&sched->from_queues[sched->core_id], actor, msg)) {
                        overflow_append(sched->core_id, actor, msg);
                    }
                }
                work_done = 1;
                continue;
            }

            // Drain actor mailbox aggressively BEFORE trying to add new message
            if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > MAILBOX_SIZE / 2) {
                int drained = 0;
                if (actor->step && !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
                    uint64_t _drain_start = atomic_load_explicit(&g_aether_config.preempt_enabled, memory_order_relaxed) ? aether_now_ns() : 0;
                    while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 && drained < 128) {
                        if (unlikely(atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) != sched->core_id))
                            break;
                        aether_step_safe(actor);
                        drained++;
                        // Preemption: yield if handler batch exceeds threshold
                        if (_drain_start && (aether_now_ns() - _drain_start) >= g_aether_config.preempt_threshold_ns)
                            break;
                    }
                    atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
                }
                sched->messages_processed += drained;
            }

            // Re-check: actor may have been stolen during aggressive drain
            if (unlikely(atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) != sched->core_id)) {
                int new_core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
                if (new_core >= 0 && new_core < num_cores) {
                    int forwarded = 0;
                    for (int r = 0; r < 8; r++) {
                        if (queue_enqueue(&schedulers[new_core].from_queues[sched->core_id], actor, msg)) {
                            forwarded = 1; break;
                        }
                        int cur = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
                        if (cur >= 0 && cur < num_cores) new_core = cur;
                        AETHER_PAUSE();
                    }
                    if (!forwarded) overflow_append(new_core, actor, msg);
                } else {
                    if (!mailbox_send(&actor->mailbox, msg)) {
                        if (!queue_enqueue(&sched->from_queues[sched->core_id], actor, msg)) {
                            overflow_append(sched->core_id, actor, msg);
                        }
                    }
                    atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
                }
                work_done = 1;
                continue;
            }

            // Now try to deliver message and immediately process.
            // FUSED DELIVER+PROCESS: Instead of delivering all messages first and
            // then scanning all actors (O(N) with N=total actors per core), we
            // deliver to the mailbox and immediately process if this actor is local
            // and unlocked.  This eliminates the second O(N) pass entirely, making
            // the scheduler loop O(batch_size) instead of O(actor_count).
            if (!mailbox_send(&actor->mailbox, msg)) {
                if (!queue_enqueue(&sched->from_queues[sched->core_id], actor, msg)) {
                    overflow_append(sched->core_id, actor, msg);
                }
            } else {
                atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
                work_done = 1;

                // Immediate processing: if actor is on our core and step_lock
                // is available, drain its mailbox right here.
                if (likely(actor->step) &&
                    !actor->auto_process &&
                    !atomic_load_explicit(&actor->main_thread_only, memory_order_acquire) &&
                    atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == sched->core_id &&
                    !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
                    int processed = 0;
                    while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 &&
                           processed < 64 &&
                           atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == sched->core_id) {
                        aether_step_safe(actor);
                        processed++;
                        // Yield to outer loop if cross-core messages arrived
                        // (e.g. StopAnimation sent from main while actor sleeps).
                        if (has_pending_cross_core(sched)) break;
                    }
                    atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
                    sched->messages_processed += processed;
                    if (processed > 0 &&
                        atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) == 0) {
                        atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
                    }
                }
            }
        }

        // TIER 1 ALWAYS ON: Adjust adaptive batch size based on what we received
        adaptive_batch_adjust(&sched->batch_state, sched->coalesce_buffer.count);

        // Check migration for actors we just processed in the coalesce buffer.
        // This is O(coalesce_count) not O(actor_count) — cheap and targeted.
        // Without this, cross-core topologies (e.g. ring) never converge.
        for (int mi = 0; mi < sched->coalesce_buffer.count; mi++) {
            ActorBase* mig_actor = (ActorBase*)sched->coalesce_buffer.actors[mi];
            int mig_to = atomic_load_explicit(&mig_actor->migrate_to, memory_order_relaxed);
            if (unlikely(mig_to >= 0 && mig_to != sched->core_id && mig_to < num_cores &&
                         atomic_load_explicit(&mig_actor->assigned_core, memory_order_relaxed) == sched->core_id)) {
                Scheduler* dst = &schedulers[mig_to];
                OptimizedSpinlock* first_lock  = (sched->core_id < mig_to) ? &sched->actor_lock : &dst->actor_lock;
                OptimizedSpinlock* second_lock = (sched->core_id < mig_to) ? &dst->actor_lock : &sched->actor_lock;
                if (!atomic_flag_test_and_set_explicit(&first_lock->lock, memory_order_acquire)) {
                    if (!atomic_flag_test_and_set_explicit(&second_lock->lock, memory_order_acquire)) {
                        if (dst->actor_count < dst->capacity) {
                            // Find and remove actor from our list
                            for (int ai = 0; ai < sched->actor_count; ai++) {
                                if (sched->actors[ai] == mig_actor) {
                                    sched->actors[ai] = sched->actors[--sched->actor_count];
                                    atomic_store_explicit(&mig_actor->assigned_core, mig_to, memory_order_relaxed);
                                    atomic_store_explicit(&mig_actor->migrate_to, -1, memory_order_relaxed);
                                    dst->actors[dst->actor_count++] = mig_actor;
                                    break;
                                }
                            }
                        }
                        atomic_flag_clear_explicit(&second_lock->lock, memory_order_release);
                    }
                    atomic_flag_clear_explicit(&first_lock->lock, memory_order_release);
                }
            }
        }

        // Scan actor list for SPSC messages and migration when idle.
        // With heavy from_queue traffic, migration is already handled above.
        if (sched->coalesce_buffer.count == 0) {
        atomic_thread_fence(memory_order_acquire);
        ActorBase** local_actors     = sched->actors;
        int         local_actor_count = sched->actor_count;
        for (int i = 0; i < local_actor_count; i++) {
            ActorBase* actor = local_actors[i];

            if (unlikely(!actor)) continue;

            // Skip actors processed on main thread
            if (unlikely(atomic_load_explicit(&actor->main_thread_only, memory_order_acquire))) continue;
            if (unlikely(actor->auto_process)) {
                work_done = 1;
                continue;
            }

            // Skip actors with no pending messages AND no migration request.
            // This is the key optimization: with 200k+ actors per workload,
            // most actors are idle.  Checking mailbox.count (atomic, relaxed)
            // and migrate_to (atomic, relaxed) is two loads per actor vs the
            // full processing path.  This makes the loop O(active) not O(total).
            int mbox_count = atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed);
            int mig_to = atomic_load_explicit(&actor->migrate_to, memory_order_relaxed);
            if (mbox_count == 0 && mig_to < 0) continue;  // Skip inactive actors (fast path!)

            int was_active = atomic_load_explicit(&actor->active, memory_order_relaxed);
            if (!was_active) {
                atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
                was_active = 1;
            }

            if (likely(actor->step)) {
                // Drain SPSC queue (lock-free same-core messages, only for auto_process actors)
                Message spsc_msgs[128];
                int spsc_count = actor->spsc_queue
                    ? spsc_dequeue_batch(actor->spsc_queue, spsc_msgs, 128) : 0;
                if (spsc_count > 0) {
                    int sent_count = mailbox_send_batch(&actor->mailbox, spsc_msgs, spsc_count);
                    for (int m = sent_count; m < spsc_count; m++) {
                        if (!queue_enqueue(&sched->from_queues[sched->core_id], actor, spsc_msgs[m])) {
                            overflow_append(sched->core_id, actor, spsc_msgs[m]);
                        }
                    }
                }

                int processed = 0;
                if (!atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
                    while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 &&
                           processed < 64 &&
                           atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == sched->core_id) {
                        aether_step_safe(actor);
                        processed++;
                        // Yield to outer loop if cross-core messages arrived
                        // (e.g. StopAnimation sent from main while actor sleeps).
                        if (has_pending_cross_core(sched)) break;
                    }
                    atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
                }
                sched->messages_processed += processed;
                if (processed > 0 &&
                    atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) == 0 &&
                    atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == sched->core_id) {
                    atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
                }
            }
            work_done = 1;

            // THEN: Message-driven migration — move actor to the core that
            // communicates with it most.  Processing first ensures the actor
            // makes progress even under constant migration pressure.
            //
            // SAFETY: Only migrate actors that have been initialized (have
            // messages in their mailbox, are currently active, OR were active
            // at the start of this iteration).  The third condition restores
            // co-location: after processing, active=0 and mailbox=0, but
            // was_active=1 means the setup-race window has already closed.
            // A truly freshly-spawned actor (never activated) has was_active=0,
            // active=0, mailbox=0 — migration is still blocked for that case.
            // Re-read migrate_to (may have changed during processing)
            mig_to = atomic_load_explicit(&actor->migrate_to, memory_order_relaxed);
            if (unlikely(mig_to >= 0 &&
                         mig_to != sched->core_id &&
                         mig_to < num_cores &&
                         (atomic_load_explicit(&actor->active, memory_order_relaxed) || atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 || was_active))) {
                int dst_core = mig_to;
                Scheduler* dst = &schedulers[dst_core];

                // Lock in ascending core-id order to prevent deadlock
                OptimizedSpinlock* first_lock  = (sched->core_id < dst_core) ? &sched->actor_lock : &dst->actor_lock;
                OptimizedSpinlock* second_lock = (sched->core_id < dst_core) ? &dst->actor_lock : &sched->actor_lock;

                if (!atomic_flag_test_and_set_explicit(
                        &first_lock->lock, memory_order_acquire)) {
                    if (!atomic_flag_test_and_set_explicit(
                            &second_lock->lock, memory_order_acquire)) {
                        if (dst->actor_count < dst->capacity) {
                            ActorBase* replacement = sched->actors[--sched->actor_count];
                            sched->actors[i] = replacement;
                            local_actors[i]  = replacement;  // keep snapshot in sync
                            atomic_store_explicit(&actor->assigned_core, dst_core, memory_order_relaxed);
                            atomic_store_explicit(&actor->migrate_to, -1, memory_order_relaxed);
                            dst->actors[dst->actor_count++] = actor;

                            atomic_flag_clear_explicit(&second_lock->lock, memory_order_release);
                            atomic_flag_clear_explicit(&first_lock->lock, memory_order_release);

                            i--;  // Re-examine this slot (now holds a different actor)
                            continue;
                        }
                        atomic_flag_clear_explicit(&second_lock->lock, memory_order_release);
                    }
                    atomic_flag_clear_explicit(&first_lock->lock, memory_order_release);
                }
            }
        }
        }  // end gated actor scan

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

                    if (!atomic_flag_test_and_set_explicit(&first_lock->lock, memory_order_acquire)) {
                        if (!atomic_flag_test_and_set_explicit(&second_lock->lock, memory_order_acquire)) {
                            if (victim->actor_count > 4 && sched->actor_count < sched->capacity) {
                                // Search backward for a stealable actor.  Only steal actors
                                // that have been activated (active flag set, or messages in
                                // mailbox).  A freshly spawned actor whose first messages
                                // are still in from_queues must NOT be stolen: changing its
                                // assigned_core mid-flight would route subsequent messages
                                // (e.g. Spawn) to the new core while earlier messages
                                // (e.g. Setup) are still queued on the old core, breaking
                                // the FIFO ordering that actors depend on for initialization.
                                ActorBase* stolen = NULL;
                                for (int s = victim->actor_count - 1; s >= 4; s--) {
                                    ActorBase* candidate = victim->actors[s];
                                    if (!atomic_load_explicit(&candidate->active, memory_order_relaxed) &&
                                        atomic_load_explicit(&candidate->mailbox.count,
                                                             memory_order_relaxed) == 0) {
                                        continue;  // freshly spawned or fully idle — skip
                                    }
                                    // Swap candidate to the end and steal it
                                    victim->actors[s] = victim->actors[victim->actor_count - 1];
                                    victim->actor_count--;
                                    stolen = candidate;
                                    break;
                                }
                                if (stolen) {
                                    atomic_store_explicit(&stolen->assigned_core, sched->core_id, memory_order_relaxed);
                                    atomic_store_explicit(&stolen->migrate_to, -1, memory_order_relaxed);
                                    sched->actors[sched->actor_count++] = stolen;
                                    work_done = 1;
                                    atomic_fetch_add(&sched->steal_attempts, 1);
                                }
                            }
                            atomic_flag_clear_explicit(&second_lock->lock, memory_order_release);
                        }
                        atomic_flag_clear_explicit(&first_lock->lock, memory_order_release);
                    }
                }
            }

            // Keep spinning aggressively for high-throughput workloads
            if (idle_count < 10000) {
                // Tight spin with architecture-specific pause
                AETHER_PAUSE();
            } else {
                // Extended idle: use I/O poller with short timeout instead of blind yield
                int io_events = scheduler_io_poll(sched, 1);
                if (io_events > 0) {
                    idle_count = 0;  // I/O activity — go back to active processing
                } else {
                    aether_sched_yield();
                    idle_count = 5000;
                }
            }
        } else {
            idle_count = 0;
            atomic_store_explicit(&sched->idle_cycles, 0, memory_order_relaxed);
        }
        
        // Explicit check to ensure timely exit on all platforms
        if (!atomic_load_explicit(&sched->running, memory_order_acquire)) {
            break;
        }
    }

    // Final flush: drain any remaining overflow entries before thread exits.
    // scheduler_wait waits for count_pending_messages()==0 (which includes
    // g_overflow_total) before calling scheduler_stop, so normally overflow is
    // empty here.  But under extreme contention a late overflow_append could
    // slip in — flush it so no messages are lost.
    overflow_flush(sched->core_id);

    aether_unwind_thread_cleanup();

    return NULL;
}

void scheduler_init(int cores) {
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

    // TIER 2: Auto-detect hardware capabilities first
    aether_detect_hardware();

    // Initialize profile and inline mode from env vars
    aether_init_from_env();

    // Reset main-thread-mode and actor count between scheduler lifecycles.
    // Tests call scheduler_init/cleanup in sequence; a prior run may have left
    // main_thread_mode=true (via scheduler_spawn_actor), causing scheduler_start()
    // and scheduler_wait() to skip creating/joining threads on the next run.
    // Only reset if the user hasn't force-enabled inline mode via AETHER_INLINE.
    if (!atomic_load_explicit(&g_aether_config.inline_mode_forced, memory_order_relaxed)) {
        atomic_store_explicit(&g_aether_config.main_thread_mode, false, memory_order_relaxed);
        atomic_store_explicit(&g_aether_config.inline_mode_active, false, memory_order_relaxed);
        g_aether_config.main_actor = NULL;
    }
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
        schedulers[i].actors = aether_numa_alloc(MAX_ACTORS_PER_CORE * sizeof(ActorBase*), numa_node);
        if (!schedulers[i].actors) {
            fprintf(stderr, "ERROR: Failed to allocate memory for scheduler %d actors\n", i);
            exit(1);
        }
        // Zero the slot array so uninitialized entries read as NULL.
        // aether_numa_alloc() falls back to malloc() (not calloc) without libnuma,
        // leaving slots with garbage.  The scheduler loop's null-check relies on
        // this to guard against stale actor_count reads on ARM64.
        memset(schedulers[i].actors, 0, MAX_ACTORS_PER_CORE * sizeof(ActorBase*));
        schedulers[i].actor_count = 0;
        schedulers[i].capacity = MAX_ACTORS_PER_CORE;
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
    }
}

// Initialize with explicit optimization flags (TIER 3 opt-in)
void scheduler_init_with_opts(int cores, AetherOptFlags opts) {
    aether_runtime_configure(opts);
    scheduler_init(cores);
}

void scheduler_start() {
    // MAIN THREAD MODE: Single-actor programs don't need scheduler threads
    // All message processing happens synchronously on the main thread
    if (aether_main_thread_mode_active()) {
        return;  // No threads to start
    }

    // Only start threads once (idempotent: safe to call multiple times)
    if (atomic_exchange_explicit(&g_threads_started, 1, memory_order_acq_rel) == 1) {
        return;  // Threads already running
    }

    // Reset per this start() call in case scheduler_init was not called again.
    atomic_store_explicit(&g_threads_ready, 0, memory_order_relaxed);

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

// Ensure scheduler threads are running. Called when transitioning out of
// main-thread mode (e.g. self-send from an actor handler). In main-thread
// mode, scheduler_start() skips thread creation; this function starts them
// on demand so queued messages get processed.
void scheduler_ensure_threads_running(void) {
    if (atomic_load_explicit(&g_threads_started, memory_order_acquire)) {
        return;  // Already running
    }
    scheduler_start();
}

void scheduler_stop() {
    // Set all running flags to 0 with release semantics
    for (int i = 0; i < num_cores; i++) {
        atomic_store_explicit(&schedulers[i].running, 0, memory_order_release);
    }
    
    // Wake threads by writing to monitored addresses (wakes MWAIT)
    // On non-MWAIT platforms, threads wake quickly from short sleep
    for (int i = 0; i < num_cores; i++) {
        // Touch the main-thread from_queue tail to trigger MWAIT wake
        atomic_store_explicit(&schedulers[i].from_queues[MAX_CORES].tail,
                            atomic_load_explicit(&schedulers[i].from_queues[MAX_CORES].tail, memory_order_relaxed),
                            memory_order_release);
    }
}

// Count total pending messages across all from_queues, TLS overflow buffers,
// AND the sent-vs-processed counter delta (catches messages sitting in actor
// mailboxes that aren't visible in queue/overflow counts).
// Called from the main thread during scheduler_wait().
static inline int count_pending_messages(void) {
    int total = 0;
    uint64_t total_sent = 0, total_proc = 0;
    for (int i = 0; i < num_cores; i++) {
        Scheduler* core = &schedulers[i];
        for (int q = 0; q <= MAX_CORES; q++) {
            total += queue_size(&core->from_queues[q]);
        }
        total_sent += core->messages_sent;
        total_proc += core->messages_processed;
    }
    // Include main thread sends
    total_sent += atomic_load_explicit(&main_thread_sent, memory_order_relaxed);
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
static int has_pending_actor_timeout(void) {
    for (int c = 0; c < num_cores; c++) {
        for (int i = 0; i < schedulers[c].actor_count; i++) {
            ActorBase* a = schedulers[c].actors[i];
            if (a && a->timeout_ns > 0) return 1;
        }
    }
    return 0;
}

void scheduler_wait() {
    // MAIN THREAD MODE: All messages processed synchronously, nothing to wait for
    // This is the fastest path for single-actor programs (counting benchmark)
    if (aether_main_thread_mode_active()) {
        // But if any actor has a pending timeout, poll until it fires
        if (has_pending_actor_timeout()) {
            int rounds = 0;
            while (has_pending_actor_timeout() && rounds < 100000) {
                for (int c = 0; c < num_cores; c++) {
                    for (int i = 0; i < schedulers[c].actor_count; i++) {
                        ActorBase* a = schedulers[c].actors[i];
                        if (a && a->timeout_ns > 0 && a->step) {
                            a->step(a);
                        }
                    }
                }
                #ifdef _WIN32
                Sleep(1);
                #else
                usleep(1000);
                #endif
                rounds++;
            }
        }
        return;
    }

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
    while (stable_count < 5) {
        atomic_thread_fence(memory_order_acquire);

        int pending = count_pending_messages();

        if (pending == 0) {
            stable_count++;
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
// started (main-thread mode) or already shut down.
void scheduler_shutdown() {
    // Wait for any in-flight messages first
    scheduler_wait();

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
}

void scheduler_cleanup() {
    // Free allocated scheduler resources
    for (int i = 0; i < num_cores; i++) {
        // Clean up thread resources
        schedulers[i].thread = 0;

        if (schedulers[i].actors != NULL) {
            // Free with the size we allocated, not current capacity
            aether_numa_free(schedulers[i].actors, MAX_ACTORS_PER_CORE * sizeof(ActorBase*));
            schedulers[i].actors = NULL;
        }
        // Clean up I/O event loop
        aether_io_poller_destroy(&schedulers[i].io_poller);
        free(schedulers[i].io_map);
        schedulers[i].io_map = NULL;
        schedulers[i].io_registered_count = 0;

        // Reset counters
        schedulers[i].actor_count = 0;
        schedulers[i].capacity = 0;
    }
    num_cores = 0;
}

int scheduler_register_actor(ActorBase* actor, int preferred_core) {
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
                          + (long)schedulers[c].actor_count;
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

    if (sched->actor_count >= sched->capacity) {
        // Dynamically grow actor array with NUMA-aware reallocation
        int numa_node = aether_numa_node_of_cpu(preferred_core);
        size_t old_size = sched->capacity * sizeof(ActorBase*);
        size_t new_size = sched->capacity * 2 * sizeof(ActorBase*);

        ActorBase** new_actors = aether_numa_alloc(new_size, numa_node);
        if (!new_actors) {
            spinlock_unlock(&sched->actor_lock);
            fprintf(stderr, "Fatal: Failed to grow actor array for core %d\n", preferred_core);
            return -1;
        }

        // Copy old data.  Do NOT free the old array here: the scheduler thread
        // that owns this core may be concurrently iterating sched->actors with
        // a snapshotted pointer (taken under acquire fence).  Freeing would
        // create a use-after-free race.  The old array is leaked until
        // scheduler_cleanup(), which frees only the current (final) pointer.
        // Waste is bounded: O(log2(final_capacity) * initial_capacity * 8 bytes)
        // per core — acceptable for typical actor counts.
        memcpy(new_actors, sched->actors, old_size);

        sched->actors = new_actors;
        sched->capacity *= 2;
    }

    atomic_store_explicit(&actor->assigned_core, preferred_core, memory_order_relaxed);

    // SPSC queue is lazy-allocated: only when auto_process is set.
    // actor->spsc_queue stays NULL for regular actors (saves 3 KB/actor).

    sched->actors[sched->actor_count++] = actor;

    spinlock_unlock(&sched->actor_lock);

    return preferred_core;
}

// Remove an actor from its scheduler's actor array.
// Must be called before freeing actor memory to avoid dangling pointers.
void scheduler_deregister_actor(ActorBase* actor) {
    if (!actor) return;
    int core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);
    if (core < 0 || core >= num_cores) return;

    Scheduler* sched = &schedulers[core];
    spinlock_lock(&sched->actor_lock);
    for (int i = 0; i < sched->actor_count; i++) {
        if (sched->actors[i] == actor) {
            sched->actors[i] = sched->actors[--sched->actor_count];
            break;
        }
    }
    spinlock_unlock(&sched->actor_lock);
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
        msg.zerocopy.owned = 0;
        msg._reply_slot = NULL;

        // One-shot: the backend auto-disables the fd after firing.
        // Mark our io_map entry inactive so we don't try to unregister again.
        entry->active = 0;
        entry->actor = NULL;
        sched->io_registered_count--;

        // Route through the scheduler's self-channel (from_queues[core_id])
        // instead of mailbox_send. The mailbox is SPSC and the accept thread
        // is already the producer via direct_send — adding a second producer
        // (this scheduler thread) would corrupt the ring buffer.
        if (!queue_enqueue(&sched->from_queues[sched->core_id], actor, msg)) {
            overflow_append(sched->core_id, actor, msg);
        }
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    }
    return n;
}

// Thread-local recursion guard for work inlining (prevent stack overflow)
static AETHER_TLS int inline_depth = 0;
#define MAX_INLINE_DEPTH 2

void scheduler_send_local(ActorBase* actor, Message msg) {
    // Drop messages to dead actors — they can't process them and we don't
    // want to grow their mailbox or fire their step().
    if (unlikely(!actor || atomic_load_explicit(&actor->dead, memory_order_acquire))) {
        return;
    }

    // NOTE: Inline mode optimization is handled by the existing WORK INLINING
    // code below (lines 590+), which is safe because it checks for scheduler
    // thread context (current_core_id >= 0 && actor->assigned_core == current_core_id).
    //
    // The profile-based inline mode (aether_inline_mode_active) is NOT used here
    // because it would cause races when main thread sends to actors being
    // processed by scheduler threads.

    // Per-core sent counter - no atomic contention on hot path!
    if (likely(current_core_id >= 0)) {
        schedulers[current_core_id].messages_sent++;
    } else {
        // Main thread (rare) - use atomic
        atomic_fetch_add_explicit(&main_thread_sent, 1, memory_order_relaxed);
    }
    // auto_process actors own their mailbox; deliver via thread-safe SPSC.
    if (unlikely(actor->auto_process)) {
        spsc_enqueue(ensure_spsc_queue(actor), msg);
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
    } else {
        // Set active=1 BEFORE the mailbox_send count++ (release).
        // mailbox_send does atomic_fetch_add(&count, release), which publishes
        // both the message data AND this active=1 to any thread that subsequently
        // reads count with acquire (mailbox_receive or the scheduler loop's
        // acquire pre-check).  This is the cross-thread happens-before required
        // for the work-stealing handoff: if the actor is stolen to another core
        // after this send, the new owner will see both active=1 and the message.
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
        if (unlikely(!mailbox_send(&actor->mailbox, msg))) {
            // Mailbox full: re-queue via self-channel or overflow buffer.
            // Without this, the message is lost but messages_sent was already
            // incremented, causing scheduler_wait to hang forever.
            int core = current_core_id;
            if (core >= 0 && core < num_cores) {
                if (!queue_enqueue(&schedulers[core].from_queues[core], actor, msg)) {
                    overflow_append(core, actor, msg);
                }
            } else {
                // Main thread: spin-retry mailbox (rare, small mailbox pressure)
                while (!mailbox_send(&actor->mailbox, msg)) { AETHER_PAUSE(); }
            }
        }
    }

    // WORK INLINING: If actor is idle and we're not too deep, run it immediately.
    // This eliminates scheduler loop overhead for tight request-response patterns.
    // Re-check assigned_core with relaxed: if work-stealing fired between the
    // mailbox write and here, assigned_core will have changed and we skip inline.
    //
    // When overflow sends are pending, still allow inlining (to make progress)
    // but flush overflow between inline calls to prevent unbounded growth.
    if (likely(current_core_id >= 0) &&
        inline_depth < MAX_INLINE_DEPTH &&
        atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == current_core_id &&
        atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) == 1 &&
        !atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
        inline_depth++;
        aether_step_safe(actor);
        schedulers[current_core_id].messages_processed++;
        if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) == 0) {
            atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
        }
        // Flush overflow accumulated during step() before next inline.
        // This prevents unbounded overflow growth from recursive inlining.
        if (tls_overflow_any) {
            overflow_flush(current_core_id);
        }
        inline_depth--;
        atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
    }
}

void scheduler_send_remote(ActorBase* actor, Message msg, int from_core) {
    // Drop messages to dead actors (see scheduler_send_local).
    if (unlikely(!actor || atomic_load_explicit(&actor->dead, memory_order_acquire))) {
        return;
    }
    // INLINE MODE: For single-actor programs, process synchronously on the main thread.
    // scheduler_send_batch_add has the same check; keep them in sync.
    if (unlikely(aether_main_thread_mode_active())) {
        mailbox_send(&actor->mailbox, msg);
        aether_step_safe(actor);
        AETHER_STAT_INC(inline_sends);
        return;
    }

    // Per-core sent counter - no atomic contention on hot path!
    if (likely(current_core_id >= 0)) {
        schedulers[current_core_id].messages_sent++;
    } else {
        // Main thread (rare) - use atomic
        atomic_fetch_add_explicit(&main_thread_sent, 1, memory_order_relaxed);
    }
    int target_core = atomic_load_explicit(&actor->assigned_core, memory_order_relaxed);

    // Guard against uninitialized or invalid assigned_core
    if (unlikely(target_core < 0 || target_core >= num_cores)) {
        target_core = actor->id % num_cores;
    }


    // Already same-core AND caller is actually running on that core's
    // scheduler thread — deliver directly to mailbox (no queue overhead).
    // The current_core_id check prevents non-scheduler threads (e.g. main)
    // from racing with the scheduler thread on mailbox access.
    if (from_core >= 0 && from_core == target_core &&
        from_core == current_core_id) {
        if (unlikely(actor->auto_process)) {
            spsc_enqueue(ensure_spsc_queue(actor), msg);
        } else {
            if (unlikely(!mailbox_send(&actor->mailbox, msg))) {
                // Mailbox full: re-queue via self-channel or overflow.
                if (!queue_enqueue(&schedulers[from_core].from_queues[from_core], actor, msg)) {
                    overflow_append(from_core, actor, msg);
                }
            }
        }
        atomic_store_explicit(&actor->active, 1, memory_order_relaxed);
        AETHER_STAT_INC(direct_sends);
        return;
    }

    // Cross-core send: migrate target actor to sender's core.
    // This converges communicating actors onto the same core, turning cross-core
    // sends into local sends.  For ring/ping-pong patterns, convergence happens
    // within a few messages.
    //
    // GUARD: Only set migrate_to if the actor has been activated (processed at least
    // one message).  Setting migrate_to on a freshly spawned actor before its first
    // message (Setup) is delivered causes a race: the scheduler thread migrates the
    // actor to the sender's core, then the next send (Spawn) takes the fast local
    // path (scheduler_send_local) and bypasses Setup still queued on the old core.
    if (from_core >= 0 && from_core == current_core_id && !actor->auto_process &&
        atomic_load_explicit(&actor->active, memory_order_relaxed)) {
        if (from_core != target_core) {
            atomic_store_explicit(&actor->migrate_to, from_core, memory_order_relaxed);
        }
    }

    // Enqueue to target core's per-sender SPSC channel (SPSC: only current_core_id writes here).
    int from_idx = (current_core_id >= 0 && current_core_id < MAX_CORES) ? current_core_id : MAX_CORES;

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
            atomic_fetch_add_explicit(&schedulers[target_core].work_count, 1,
                                       memory_order_relaxed);
            AETHER_STAT_INC(queue_sends);
            return;
        }
        AETHER_PAUSE();
    }

    // Queue still full after bounded retry.
    // Scheduler thread: defer to overflow and return — never block the loop.
    if (from_core >= 0) {
        overflow_append(target_core, actor, msg);
        AETHER_STAT_INC(queue_sends);
        return;
    }

    // Main thread (from_core < 0): original spin-retry is safe here because the
    // main thread does not drain from_queues, so it cannot be part of a circular wait.
    int retries = 0;
    while (!queue_enqueue(&schedulers[target_core].from_queues[from_idx], actor, msg)) {
        if (++retries % 1000 == 0) aether_sched_yield();
        AETHER_PAUSE();
    }
    atomic_fetch_add_explicit(&schedulers[target_core].work_count, 1, memory_order_relaxed);
    AETHER_STAT_INC(queue_sends);
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

void scheduler_send_batch_start(void) {
    if (!g_batch_buffer) {
        g_batch_buffer = calloc(1, sizeof(BatchSendBuffer));
        if (!g_batch_buffer) return;  // OOM: batching stays off, callers fall back
    }
    g_batch_buffer->count = 0;
    memset(g_batch_buffer->by_core, 0, sizeof(g_batch_buffer->by_core));
}

void scheduler_send_batch_add(ActorBase* actor, Message msg) {
    // FAST PATH: Single-actor programs bypass batching entirely
    // Main Thread Mode = synchronous processing, zero queue overhead
    if (aether_main_thread_mode_active()) {
        mailbox_send(&actor->mailbox, msg);
        aether_step_safe(actor);
        AETHER_STAT_INC(inline_sends);
        return;
    }

    // BATCH PATH: Multi-actor fan-out optimization
    if (!g_batch_buffer) {
        scheduler_send_batch_start();
        if (!g_batch_buffer) {
            // Batch buffer couldn't be allocated: deliver directly so the
            // message is never dropped. Batching is only a throughput optimization.
            mailbox_send(&actor->mailbox, msg);
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

    // === PHASE 1: Snapshot target cores and compute per-core counts ===
    // Read assigned_core ONCE per message and store it.  The by_core[] counts
    // recorded in batch_add can be stale if an actor migrated between add and
    // flush — recomputing from a single consistent snapshot prevents the
    // sorted_actors[] overflow that would result from a count/core mismatch.
    int targets[BATCH_SEND_SIZE];
    int by_core[MAX_CORES];
    memset(by_core, 0, sizeof(by_core));

    for (int i = 0; i < count; i++) {
        int tc = atomic_load_explicit(&g_batch_buffer->actors[i]->assigned_core, memory_order_relaxed);
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
        int pos = positions[targets[i]]++;
        sorted_actors[pos] = g_batch_buffer->actors[i];
        sorted_msgs[pos] = g_batch_buffer->messages[i];
    }

    // === PHASE 4: Batch enqueue to each core (ONE atomic per core!) ===
    uint64_t total_sent = 0;
    for (int c = 0; c < num_cores; c++) {
        int start = offsets[c];
        int cnt = by_core[c];
        if (cnt == 0) continue;

        // Batch send is always called from main thread (current_core_id = -1),
        // so use the main-thread SPSC channel (from_queues[MAX_CORES]).
        int enqueued = queue_enqueue_batch(
            &schedulers[c].from_queues[MAX_CORES],
            &sorted_actors[start],
            &sorted_msgs[start],
            cnt
        );

        // Fallback for overflow (rare) - scheduler_send_remote handles its own counting
        for (int j = enqueued; j < cnt; j++) {
            scheduler_send_remote(sorted_actors[start + j], sorted_msgs[start + j], -1);
        }

        // Only count batch-enqueued messages here (fallback already counted by send_remote)
        total_sent += enqueued;
        atomic_fetch_add_explicit(&schedulers[c].work_count, enqueued, memory_order_relaxed);
    }

    // Single atomic update for batch-sent messages
    atomic_fetch_add_explicit(&main_thread_sent, total_sent, memory_order_relaxed);

    // Reset buffer
    g_batch_buffer->count = 0;
    memset(g_batch_buffer->by_core, 0, sizeof(g_batch_buffer->by_core));
}

// Spawn actor with NUMA-aware allocation.  actor_size must be >= sizeof(ActorBase)
// and cover the full derived-actor struct (e.g. sizeof(PingActor)).
ActorBase* scheduler_spawn_actor(int preferred_core, void (*step)(void*), size_t actor_size) {
    if (preferred_core < 0 || preferred_core >= num_cores) {
        // Spawn on caller's core so parent→child messaging stays local.
        // Main thread (current_core_id == -1) defaults to core 0.
        preferred_core = (current_core_id >= 0) ? current_core_id : 0;
    }
    if (actor_size < sizeof(ActorBase)) actor_size = sizeof(ActorBase);

    ActorBase* actor = NULL;

    {
        int numa_node = aether_numa_node_of_cpu(preferred_core);
        actor = aether_numa_alloc(actor_size, numa_node);
        if (!actor) return NULL;
        mailbox_init(&actor->mailbox);
        AETHER_STAT_INC(actors_malloced);
    }

    actor->alloc_size = actor_size;
    actor->id = atomic_fetch_add(&next_actor_id, 1);
    actor->step = step;
    atomic_init(&actor->active, 0);  // inactive until first message send
    actor->thread = 0;
    actor->auto_process = 0;
    actor->spsc_queue = NULL;  // Lazy-allocated only for auto_process actors
    atomic_init(&actor->assigned_core, preferred_core);
    atomic_init(&actor->migrate_to, -1);
    atomic_init(&actor->main_thread_only, 0);
    atomic_init(&actor->reply_slot, NULL);
    atomic_flag_clear_explicit(&actor->step_lock, memory_order_relaxed);
    actor->timeout_ns = 0;
    actor->last_activity_ns = 0;
    atomic_init(&actor->dead, 0);

    // Track actor count for inline mode auto-detection
    // Get previous count and main_actor BEFORE aether_on_actor_spawn modifies them
    int prev_count = atomic_load_explicit(&g_aether_config.actor_count, memory_order_relaxed);
    ActorBase* prev_main_actor = (ActorBase*)g_aether_config.main_actor;
    aether_on_actor_spawn();

    // MAIN THREAD MODE handling
    if (prev_count == 0 && !atomic_load(&g_aether_config.inline_mode_disabled)) {
        // First actor: enable main thread mode for synchronous processing
        aether_enable_main_thread_mode(actor);
        atomic_store_explicit(&actor->main_thread_only, 1, memory_order_release);
    } else if (prev_count == 1 && prev_main_actor != NULL) {
        // Second actor: disable main thread mode on the first actor so scheduler
        // threads can process both actors normally.
        //
        // RACE GUARD: If the main thread is currently executing prev_main_actor's
        // step() synchronously (g_sync_step_actor == prev_main_actor), we must NOT
        // clear main_thread_only here.  Doing so would allow a scheduler thread to
        // call step() on the same actor concurrently — undefined behaviour.
        //
        // aether_send_message_sync() clears main_thread_only after step() returns,
        // so deferred clearing is always safe.
        if (g_sync_step_actor != prev_main_actor) {
            atomic_store_explicit(&prev_main_actor->main_thread_only, 0, memory_order_release);
        }
        // else: aether_send_message_sync will clear it once step() unwinds.

        // aether_on_actor_spawn() (above) disabled main_thread_mode.  If
        // scheduler_start() was already called and returned early (because
        // main-thread mode was active at the time), threads were never created.
        // Start them now so both actors can be processed by the scheduler.
        scheduler_ensure_threads_running();
    }

    scheduler_register_actor(actor, preferred_core);

    return actor;
}

void scheduler_release_actor(ActorBase* actor) {
    if (!actor) return;

    // Track actor count for inline mode auto-detection
    aether_on_actor_terminate();

    // Reclaim the lazily-allocated same-core SPSC queue (owned solely by this
    // actor, calloc'd by ensure_spsc_queue / send_buffer_flush). numa_free
    // below only releases the actor struct itself, so without this every actor
    // that ever flushed a same-core batch leaks its multi-KB queue.
    if (actor->spsc_queue) {
        free(actor->spsc_queue);
        actor->spsc_queue = NULL;
    }

    aether_numa_free(actor, actor->alloc_size);
}


// ============================================================================
// Ask/Reply support
// ============================================================================

// Thread-locals: the sender stashes a reply slot here before calling
// aether_send_message so the slot rides inside the Message._reply_slot field.
// The receiver's step function copies Message._reply_slot into
// g_current_reply_slot after mailbox_receive, so scheduler_reply can find it.
AETHER_TLS void* g_pending_reply_slot = NULL;
AETHER_TLS void* g_current_reply_slot = NULL;

static void reply_slot_decref(ActorReplySlot* slot) {
    if (atomic_fetch_sub_explicit(&slot->refcount, 1, memory_order_acq_rel) == 1) {
        pthread_cond_destroy(&slot->cond);
        pthread_mutex_destroy(&slot->mutex);
        free(slot);
    }
}

void* scheduler_ask_message(ActorBase* target, void* msg_data, size_t msg_size, int timeout_ms) {
    if (!target || !msg_data) return NULL;

    ActorReplySlot* slot = (ActorReplySlot*)calloc(1, sizeof(ActorReplySlot));
    if (!slot) return NULL;

    pthread_mutex_init(&slot->mutex, NULL);
    pthread_cond_init(&slot->cond, NULL);
    atomic_init(&slot->refcount, 2);  // asker + actor handler

    // Stash the slot in the thread-local so aether_send_message propagates it
    // into Message._reply_slot.  This supports concurrent asks to the same actor
    // because each Message carries its own slot instead of a single per-actor field.
    g_pending_reply_slot = slot;
    aether_send_message((void*)target, msg_data, msg_size);
    g_pending_reply_slot = NULL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&slot->mutex);
    while (!slot->reply_ready) {
        if (pthread_cond_timedwait(&slot->cond, &slot->mutex, &ts) == ETIMEDOUT) break;
    }
    void* result = slot->reply_ready ? slot->reply_data : NULL;

    if (!slot->reply_ready) {
        slot->timed_out = 1;
    }
    pthread_mutex_unlock(&slot->mutex);

    reply_slot_decref(slot);
    return result;
}

void scheduler_reply(ActorBase* self, void* data, size_t data_size) {
    (void)self;
    ActorReplySlot* slot = (ActorReplySlot*)g_current_reply_slot;
    g_current_reply_slot = NULL;
    if (!slot) return;

    pthread_mutex_lock(&slot->mutex);
    if (!slot->timed_out) {
        if (data && data_size > 0) {
            slot->reply_data = malloc(data_size);
            if (slot->reply_data) {
                memcpy(slot->reply_data, data, data_size);
                slot->reply_size = data_size;
            }
        }
        slot->reply_ready = 1;
        pthread_cond_signal(&slot->cond);
    }
    pthread_mutex_unlock(&slot->mutex);

    reply_slot_decref(slot);
}

// ---------------------------------------------------------------------------
// aether_scheduler_poll — drain pending messages for main-thread-only actors.
//
// Call this from C-hosted event loops (e.g., inside a render callback) to
// keep Aether actors alive while the main thread is blocked in C code.
//
// Thread safety: only touches actors flagged as main_thread_only, which
// scheduler worker threads explicitly skip — so no concurrent access.
//
// max_per_actor: max messages to process per actor per call (0 = unlimited).
// Returns total number of messages processed across all actors.
int aether_scheduler_poll(int max_per_actor) {
    // If main-thread inline mode is active, all sends are already handled
    // synchronously — nothing to drain here.
    if (aether_main_thread_mode_active()) return 0;

    int total = 0;
    int limit = (max_per_actor <= 0) ? 1024 : max_per_actor;

    for (int c = 0; c < num_cores; c++) {
        Scheduler* sched = &schedulers[c];

        // Acquire fence: ensure actor pointer writes (from registration) are visible.
        atomic_thread_fence(memory_order_acquire);
        int actor_count = sched->actor_count;

        for (int i = 0; i < actor_count; i++) {
            ActorBase* actor = sched->actors[i];
            if (!actor || !actor->step) continue;

            // Only process actors the scheduler threads are skipping.
            // It is safe to call step() here because scheduler threads check
            // main_thread_only and skip these actors entirely.
            if (!atomic_load_explicit(&actor->main_thread_only, memory_order_acquire))
                continue;

            int processed = 0;
            if (!atomic_flag_test_and_set_explicit(&actor->step_lock, memory_order_acquire)) {
                while (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) > 0 && processed < limit) {
                    aether_step_safe(actor);
                    processed++;
                    total++;
                }
                atomic_flag_clear_explicit(&actor->step_lock, memory_order_release);
            }
            if (atomic_load_explicit(&actor->mailbox.count, memory_order_relaxed) == 0)
                atomic_store_explicit(&actor->active, 0, memory_order_relaxed);
        }
    }

    return total;
}
