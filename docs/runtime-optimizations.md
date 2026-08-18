# Runtime Optimizations

## Overview

The Aether runtime implements several performance optimizations targeting message-passing overhead, synchronization primitives, and memory access patterns. Each optimization is integrated into the scheduler and message delivery paths.

The optimizations are tiered by how they activate:

- **Tier 0, platform capabilities (compile-time):** `AETHER_HAS_THREADS`, `AETHER_HAS_ATOMICS`, `AETHER_HAS_FILESYSTEM`, `AETHER_HAS_NETWORKING`, `AETHER_HAS_SIMD`, `AETHER_HAS_NUMA`, `AETHER_HAS_AFFINITY`. Auto-detected from the target; any flag can be forced off with `-DAETHER_NO_<FEATURE>` (e.g. `-DAETHER_NO_THREADING`). See [Platform Detection](architecture.md#platform-detection-tier-0).
- **Tier 1, always on:** the Active Optimizations below (direct send, adaptive batching, message coalescing, thread-local pools).
- **Tier 2, auto-detected at startup:** SIMD batch processing (AVX2/NEON), MWAIT idle (x86 MONITOR/MWAIT), CPU core pinning (OS-dependent).
- **Tier 3, opt-in:** lock-free mailboxes and message deduplication, see [Opt-In Features](#opt-in-features).

## Active Optimizations

### Main Thread Actor Mode

**Implementation:** `runtime/actors/aether_send_message.c`, `runtime/scheduler/multicore_scheduler.c`, `runtime/config/aether_optimization_config.h`

Single-actor programs bypass the scheduler entirely. When only one actor exists, message processing occurs synchronously on the main thread with zero-copy message delivery.

**Activation:**
- Automatic: Enabled when the first actor spawns, disabled if a second actor spawns
- Manual disable: Set `AETHER_NO_INLINE=1` environment variable

**Mechanism:**

When main thread mode is active:
1. `aether_send_message` calls `aether_send_message_sync` instead of routing through scheduler
2. **Threaded mode:** The sync path passes the caller's stack pointer directly (no malloc, no memcpy). A TLS flag (`g_skip_free`) prevents the handler from freeing the stack pointer
3. **Cooperative mode:** The sync path heap-allocates a copy of the message data, because the mailbox may have other messages queued ahead, the immediate `step()` call consumes the oldest message (FIFO), not necessarily the one just sent
4. `actor->step()` is called synchronously in the sender's context

```c
static inline void aether_send_message_sync(ActorBase* actor, void* message_data, size_t message_size) {
    Message msg;
    msg.type = *(int*)message_data;
#if AETHER_HAS_THREADS
    msg.payload_ptr = message_data;  // Direct pointer to caller's stack (zero-copy)
#else
    msg.payload_ptr = malloc(message_size);  // Cooperative: heap-copy (FIFO safety)
    memcpy(msg.payload_ptr, message_data, message_size);
#endif

    mailbox_send(&actor->mailbox, msg);

#if AETHER_HAS_THREADS
    g_skip_free = 1;
#endif
    actor->step(actor);
#if AETHER_HAS_THREADS
    g_skip_free = 0;
#endif
}
```

**Per-actor flag:**

The `main_thread_only` field on `ActorBase` signals scheduler threads to skip processing this actor. When a second actor spawns, the flag is cleared on the first actor so scheduler threads can process both normally. In cooperative mode, `main_thread_only` stays set for all actors, the cooperative scheduler always processes them directly.

**Scheduler integration:**

- `scheduler_start()` returns immediately if main thread mode is active (no-op in cooperative mode)
- `scheduler_wait()` returns immediately in threaded mode; in cooperative mode, drains via `aether_scheduler_poll()` loop
- Scheduler threads check `actor->main_thread_only` before processing (cooperative mode has no scheduler threads)

### Thread-Local Message Payload Pools

**Implementation:** `runtime/actors/aether_send_message.c`

Per-thread pool of pre-allocated buffers eliminates dynamic allocation overhead in the message-passing hot path. Each pool contains 256 buffers of up to 256 bytes. Messages that fit within the pool buffer size are served from the pool; larger messages fall back to `malloc`. Global atomic counters track pool hit/miss rates across threads.

```c
#define MSG_PAYLOAD_POOL_SIZE 256
#define MSG_PAYLOAD_MAX_SIZE 256

typedef struct {
    char buffer[MSG_PAYLOAD_MAX_SIZE];
    int in_use;  // Thread-local: no atomics needed
} PooledPayload;

typedef struct {
    PooledPayload payloads[MSG_PAYLOAD_POOL_SIZE];
    int next_index;  // Thread-local: plain increment
    int initialized;
} PayloadPool;

static __thread PayloadPool* g_payload_pool = NULL;
```

Because each pool is thread-local, acquisition and release use plain loads and stores rather than atomic operations. The round-robin index uses bitwise AND masking for constant-time slot lookup.

### Message Coalescing with Batch Dequeue

**Implementation:** `runtime/scheduler/multicore_scheduler.c`, `runtime/scheduler/lockfree_queue.h`

The scheduler drains multiple messages from the lock-free incoming queue in a single batch dequeue operation. This reduces atomic operations from one-per-message to one-per-batch.

```c
#define COALESCE_THRESHOLD 512

// Single batch dequeue: 1 atomic store for entire batch
count = queue_dequeue_batch(&incoming_queue, buffer.actors, buffer.messages, batch_size);

for (int i = 0; i < count; i++) {
    // Redirect migrated actors, deliver to mailbox, or enqueue to SPSC
}
```

The batch dequeue reads `head` and `tail` once, copies all available messages, then advances `head` with a single `atomic_store`. This matches the existing batch enqueue pattern used by `queue_enqueue_batch`.

### Batch Send for Fan-Out Patterns

**Implementation:** `runtime/scheduler/multicore_scheduler.c`, `compiler/codegen/codegen.c`

For main thread fan-out patterns (fork-join), batch send reduces atomic operations from N to num_cores. The compiler detects while loops containing sends in `main()` and wraps them with batch start/flush calls.

```c
// Generated code for fork-join pattern:
scheduler_send_batch_start();
while (i < total) {
    worker ! Work { value: i };  // Buffered, not sent immediately
    i = i + 1;
}
scheduler_send_batch_flush();  // Bulk send with one atomic per core
```

The flush snapshots each message's target core, sorts by core using radix sort, then calls `queue_enqueue_batch` for each core. This reduces atomics from N (one per message) to num_cores (one per core). Target cores are read once per message at flush time to produce a consistent snapshot, this prevents buffer overflows that could occur if an actor migrates between the time a message is buffered and the time the batch is flushed.

**Partial batch enqueue:** `queue_enqueue_batch` enqueues as many messages as fit in the queue and returns the count, rather than failing the entire batch when the queue is near full. This lets the common case (queue has room) complete with a single atomic store per core. When the queue can't absorb the full batch, the flush falls back to `scheduler_send_remote` for each remaining message, which handles its own routing and counting. The fallback path is rare in practice because the cross-core queue holds 1024 items.

**Runtime auto-detection:** The batch send path automatically detects when Main Thread Actor Mode is active (single-actor programs) and uses the synchronous zero-copy path instead of batching. This ensures single-actor benchmarks like counting use the optimal path while multi-actor fan-out patterns like fork-join benefit from batch send. No manual configuration is required.

**Pattern detection:** Only applied in `main()` function loops, not inside actor receive handlers. This preserves low-latency actor-to-actor messaging while optimizing main-to-actors fan-out.

### Adaptive Batch Size

**Implementation:** `runtime/actors/aether_adaptive_batch.h`

The batch size adjusts dynamically based on queue utilization. Under sustained load the batch size increases (up to 1024) to amortize overhead. During idle periods it decreases (down to 64) to maintain responsiveness.

```c
#define MIN_BATCH_SIZE 64
#define MAX_BATCH_SIZE 1024
```

### Optimized Spinlock with Platform-Specific Yield

**Implementation:** `runtime/scheduler/multicore_scheduler.h`

Custom spinlock using `atomic_flag` with platform-specific CPU yield hints during spin-wait.

```c
static inline void spinlock_lock(OptimizedSpinlock* lock) {
    while (atomic_flag_test_and_set_explicit(&lock->lock, memory_order_acquire)) {
        #if defined(__x86_64__) || defined(_M_X64)
        __asm__ __volatile__("pause" ::: "memory");
        #elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
        #endif
    }
}
```

The `PAUSE` instruction (x86) and `YIELD` instruction (ARM) reduce power consumption during spin-wait and signal the CPU that the thread is in a spin-loop, improving SMT scheduling.

### Lock-Free Cross-Core Queue

**Implementation:** `runtime/scheduler/lockfree_queue.h`

Single-producer, single-consumer ring buffer using atomic head/tail pointers with explicit memory ordering.

```c
typedef struct __attribute__((aligned(64))) {
    atomic_int head;
    char padding1[60];  // Cache line alignment
    atomic_int tail;
    char padding2[60];
    QueueItem items[QUEUE_SIZE];  // QUEUE_SIZE = 1024
} LockFreeQueue;
```

Cache line padding on `head` and `tail` prevents false sharing between producer and consumer cores. Power-of-2 sizing enables bitwise AND masking instead of modulo division.

### SPSC Queue for Same-Core Messaging

**Implementation:** `runtime/actors/aether_spsc_queue.h`

`auto_process` actors get a dedicated SPSC (single-producer, single-consumer) queue for receiving messages. The queue is lazy-allocated by `ensure_spsc_queue` on the first enqueue; `scheduler_spawn_actor` initializes `actor->spsc_queue` to `NULL`, so regular actors never allocate one and receive through their mailbox instead. This separates the auto-process delivery path from ordinary mailbox delivery.

### Direct Mailbox Delivery

**Implementation:** `runtime/scheduler/multicore_scheduler.c` (`scheduler_send_remote`)

When the sender is on the same core as the target actor and is actually running on that core's scheduler thread (verified via `current_core_id`), messages bypass the incoming queue entirely and write directly to the actor's mailbox or SPSC queue.

```c
if (from_core >= 0 && from_core == target_core &&
    from_core == current_core_id) {
    if (unlikely(actor->auto_process)) {
        spsc_enqueue(&actor->spsc_queue, msg);
    } else {
        mailbox_send(&actor->mailbox, msg);
    }
    actor->active = 1;
    return;
}
```

The `current_core_id` guard prevents non-scheduler threads (such as the main thread) from writing to the mailbox concurrently with the scheduler thread, which would be a data race on the non-thread-safe ring buffer.

### Message-Driven Actor Migration

**Implementation:** `runtime/scheduler/multicore_scheduler.c` (`scheduler_send_remote`, scheduler actor loop)

When a cross-core send occurs, the sender sets a `migrate_to` hint on the target actor, requesting migration to the sender's core. The scheduler thread that owns the actor checks migration hints in two places:

1. **After coalesce-buffer processing** (targeted, O(batch_size)): Immediately after delivering and processing messages from the coalesce buffer, the scheduler checks `migrate_to` on each actor it just processed. This provides fast migration response without scanning the full actor list.

2. **During idle actor scan** (full scan, O(actor_count)): When no messages arrived from cross-core queues, the scheduler scans all local actors for pending migration hints. This catches any hints that were not processed in the targeted path.

Migration uses ascending core-id lock ordering to prevent deadlock between concurrent migration and work-stealing operations. Both locks are acquired via non-blocking try-lock, if either lock is contended, migration is deferred to the next iteration. The actor is always processed before migration is attempted, ensuring progress even under constant migration pressure.

### Inline Single-Field Messages

**Implementation:** `compiler/codegen/codegen.c`

The code generator detects messages with exactly one scalar field that fits in `intptr_t` and emits an inline fast path. Instead of heap-allocating the message payload (malloc + memcpy + free), the field value is stored directly in `Message.payload_int`. The receiver reconstructs the struct on the stack. This eliminates all heap allocation for single-field messages.

**Supported field types** (from `is_inlineable_scalar`): `int`, `int64`, `uint64`, `duration`, and `ptr`. Other types, including `bool` and `actor_ref`, are not inlined and take the heap-allocated payload path.

**Impact:** Eliminates malloc/free for single-field messages, which are the most common pattern in tree-spawn and request-response workloads. Run `make benchmark` to measure the effect on your hardware.

### Computed Goto Dispatch

**Implementation:** `compiler/codegen/codegen_actor.c` (generated code)

The code generator emits a `dispatch_table[256]` with GCC computed goto (`goto *dispatch_table[_msg_id]`) for message handler selection. This replaces indirect function calls or switch statements with direct label jumps. The message ID is read from `msg.type` (`int _msg_id = msg.type;`) rather than dereferencing the payload pointer.

### Progressive Backoff Strategy

**Implementation:** `runtime/scheduler/multicore_scheduler.c`

Three-phase idle strategy:

1. **Tight spin with PAUSE/YIELD:** Low latency, used for the first 10,000 idle iterations
2. **Work stealing:** After 5,000 idle cycles, scan for busy cores and steal actors
3. **OS yield:** After 10,000 idle iterations, call `sched_yield()` and partially reset the counter

```c
if (idle_count < 10000) {
    #if defined(__x86_64__) || defined(_M_X64)
    __asm__ __volatile__("pause" ::: "memory");
    #endif
} else {
    sched_yield();
    idle_count = 5000;  // Partial reset to stay responsive
}
```

### Cache Line Alignment

**Implementation:** Multiple components

Frequently-accessed shared data structures are aligned to 64-byte cache line boundaries to prevent false sharing.

```c
typedef struct __attribute__((aligned(64))) {
    atomic_flag lock;
    char padding[63];
} OptimizedSpinlock;

#define MAILBOX_SIZE 32   // 32 slots, small per-actor footprint for scaling to millions
```

### Power-of-2 Buffer Sizing

**Implementation:** All ring buffers

All ring buffers use power-of-2 sizes with bitwise AND masking instead of modulo division.

```c
#define QUEUE_SIZE 1024
#define QUEUE_MASK (QUEUE_SIZE - 1)
int index = (head + 1) & QUEUE_MASK;

// Also used in thread-local pool (plain increment, no atomics)
int idx = pool->next_index++ & (MSG_PAYLOAD_POOL_SIZE - 1);
```

### Relaxed Atomic Memory Ordering

**Implementation:** `runtime/actors/aether_send_message.c`, `runtime/scheduler/multicore_scheduler.c`

Non-critical atomic operations use `memory_order_relaxed` to avoid unnecessary memory barriers:

```c
// Statistics counters
atomic_fetch_add_explicit(&g_pool_hits, 1, memory_order_relaxed);

// Scheduler idle tracking
atomic_fetch_add_explicit(&sched->idle_cycles, 1, memory_order_relaxed);
atomic_store_explicit(&sched->idle_cycles, 0, memory_order_relaxed);
```

Statistics counters and approximate work counts do not require sequential consistency. Relaxed ordering provides atomicity without the overhead of full memory barriers.

### NUMA-Aware Allocation

**Implementation:** `runtime/aether_numa.c`, `runtime/aether_numa.h`

Actor structures are allocated on the NUMA node local to the assigned core. The topology is detected at scheduler initialization. On systems without NUMA support, allocation falls back to standard `malloc`.

- **Linux:** `numa_alloc_onnode` (requires libnuma)
- **Windows:** `VirtualAllocExNuma`
- **Single-node systems:** Graceful degradation to `malloc`

### Link-Time Optimization

**Implementation:** `benchmarks/cross-language/aether/Makefile`

The benchmark build uses `-flto` for both compilation and linking, enabling cross-translation-unit inlining and dead code elimination.

### CPU Detection Fallback

**Implementation:** `runtime/utils/aether_cpu_detect.c`

The `cpu_recommend_cores` function uses CPUID on x86 and falls back to OS-level APIs (`sysctl` on macOS, `sysconf` on Linux, `GetNativeSystemInfo` on Windows) when CPUID returns zero (ARM, virtualized environments).

### Apple Silicon P-Core Detection

**Implementation:** `runtime/utils/aether_cpu_detect.c`, `runtime/scheduler/multicore_scheduler.c`

On Apple Silicon (M1/M2/M3), the runtime detects and uses only Performance cores (P-cores) to ensure consistent throughput. Efficiency cores (E-cores) are excluded because they run at lower clock speeds and can cause variance in benchmarks.

Detection uses `sysctlbyname("hw.perflevel0.physicalcpu")` to query the P-core count. Combined with `QOS_CLASS_USER_INTERACTIVE` thread priority, this encourages macOS to schedule actor threads on P-cores.

**Platform-specific thread affinity:**

| Platform | Mechanism | Binding |
|----------|-----------|---------|
| Linux | `pthread_setaffinity_np` | Hard binding |
| macOS | `thread_policy_set` + QoS | Hint only |
| Windows | `SetThreadAffinityMask` | Hard binding |

Note: macOS does not support hard core pinning by design. Thread placement is advisory, which may cause occasional variance in microbenchmarks.

### Per-Core Message Counters

**Implementation:** `runtime/scheduler/multicore_scheduler.c`, `runtime/scheduler/multicore_scheduler.h`

Idle detection uses per-core counters instead of a global atomic counter. This eliminates cache line contention on the message-passing hot path.

```c
typedef struct {
    // ...
    _Atomic uint64_t messages_sent;      // Messages sent FROM this core
    _Atomic uint64_t messages_processed; // Messages processed ON this core
    char counter_padding[48];    // Cache line padding
    // ...
} Scheduler;
```

Each scheduler core increments only its own counters, so there is no cross-core write contention on the hot path. The counters are declared `_Atomic uint64_t` because they are read cross-thread by the idle check, so the `++` compiles to a relaxed atomic read-modify-write rather than a barrier-heavy sequentially-consistent one; this keeps torn reads off weakly-ordered architectures (e.g. ARM64) without adding contention. The `wait_for_idle()` Aether builtin lowers to the C function `scheduler_wait()`, which sums across all cores to determine when all in-flight messages have been processed. `scheduler_wait()` is non-destructive -- it only waits for quiescence and does not stop scheduler threads, so it can be called multiple times between phases of message sending.

```c
// Hot path: no atomic contention
if (current_core_id >= 0) {
    schedulers[current_core_id].messages_sent++;
}

// scheduler_wait(): sum across cores (rare operation)
uint64_t total_sent = 0, total_processed = 0;
for (int i = 0; i < num_cores; i++) {
    total_sent += schedulers[i].messages_sent;
    total_processed += schedulers[i].messages_processed;
}
```

Messages sent from the main thread (before scheduler threads start) use a separate atomic counter, but this path is infrequent. The pattern follows the Linux kernel's per-CPU counter design for scalable counting.

## Rejected Optimizations

### Manual Prefetching

Benchmarks showed hardware prefetchers handle sequential ring buffer access more effectively than manual `__builtin_prefetch` hints. Manual prefetch introduced pipeline stalls.

### SIMD Message Processing

Message processing is memory-bound. Vectorization overhead exceeded benefits for typical message sizes.

### Actor Pooling (#1332)

Reusing actor allocations across spawn/destroy churn, so a high-churn workload
stops paying malloc/free per actor. Measured before implementing, per the rule
that perf work needs profiling evidence first, and the measurement did not
support it.

Workload: the skynet benchmark at `SKYNET_LEAVES=1000000000`, which constructs
**11.1M actors**, the most spawn-heavy workload in the tree. Sampled with
`sample(1)` on macOS/arm64. Idle frames (`swtch_pri`, `__semwait_signal`,
`scheduler_wait`) are excluded from the denominator, since a pool cannot recover
time nobody is using.

Sampling the **tree-construction phase specifically**, which is where every
spawn happens:

| Frame | Share of non-idle |
|---|---|
| `scheduler_thread` (the scheduler loop) | 70.0% |
| `scheduler_spawn_actor` (self) | 10.1% |
| `tlv_get_addr` (thread-local access) | 6.7% |
| `scheduler_io_poll` | 4.8% |
| **malloc / free, all frames** | **2.9%** |
| `scheduler_register_actor` | 0.2% |

Sampling a whole run mid-flight instead, so steady-state messaging dominates,
puts the allocator at **0.3-0.5%** of non-idle time across two runs.

So the ceiling for actor pooling is roughly 3% of non-idle CPU in the
construction phase of a benchmark built to do nothing but construct actors, and
well under 1% of a realistic run. A pool cannot capture even that much: it still
has to re-initialize the mailbox, atomics, `alloc_size` and the panic/dead flags,
which is most of what `scheduler_spawn_actor`'s own 10% is doing. The allocation
call is the small half of the small number.

Against that, a correct pool has to be size-class bucketed (actors are
variable-size derived structs), preserve NUMA affinity, and handle release
happening on a different core than spawn because work stealing migrates actors
mid-life. The previous attempt (removed in PR #1330) got the layout wrong and
routed NUMA-allocated memory to plain `free()`.

Not worth it at this ratio. Revisit if a profile ever shows allocation as a
double-digit share.

**Side finding worth its own look:** `tlv_get_addr` is 6.7% of non-idle time,
stable across all three samples, and about twice the allocator. Thread-local
variable access is a larger cost in the actor hot path than actor allocation
ever was.

## Opt-In Features

The following optimizations are available but disabled by default. Enable them via runtime configuration flags when appropriate for your workload.

**Message Deduplication** (`runtime/actors/aether_message_dedup.h`)
- Filters duplicate messages using a sliding window of message fingerprints
- Enable via `AETHER_OPT_MESSAGE_DEDUP` flag
- Trade-off: adds overhead, changes message delivery semantics

**Lock-Free Mailbox** (`runtime/actors/lockfree_mailbox.h`)
- SPSC atomic queue replacing standard ring buffer
- Enable via `AETHER_OPT_LOCKFREE_MAILBOX` flag
- Trade-off: slower for single-threaded workloads, faster under heavy contention

**SIMD Batch Processing** (`runtime/actors/aether_simd_batch.h`)
- Vectorized message processing using AVX2 or NEON
- Auto-detected based on hardware capabilities
- Trade-off: overhead exceeds benefit for small message batches

## Compiler Optimizations

The Aether compiler (`aetherc`) runs its own optimization passes on the AST before emitting C, separate from whatever the downstream C compiler does. All passes are safe-by-default: if a pattern is not recognized, the compiler falls through to normal code generation.

### Hot/Cold Path Fixes (scheduler-level)

**Problem A, `aether_main_thread_mode_active()` branch hint** (`runtime/config/aether_optimization_config.h`)

The inline accessor previously carried `__builtin_expect(..., 0)`, marking the main-thread path as *unlikely*. For single-actor programs this path is always taken, so the hint put the fast code in the cold instruction-cache section. Removed the hint; the C compiler now places the branch neutrally.

**Problem B, Dead branch in generated send code** (`compiler/codegen/codegen_expr.c`)

Generated code for `actor ! Msg` from the main thread previously emitted:
```c
if (current_core_id >= 0 && current_core_id == atomic_load_explicit(&actor->assigned_core, memory_order_relaxed)) {
    scheduler_send_local(...);   // never taken: main thread has core_id = -1
} else {
    scheduler_send_remote(...);  // always taken
}
```
When the codegen is in a main-function context (`gen->current_actor == NULL`), it now emits the always-taken branch directly, eliminating the dead conditional.

### Arithmetic Series Loop Collapse (`compiler/codegen/codegen_stmt.c`)

Detects `while` loops of the form:

```aether,fragment
while counter < N {
    acc1 = acc1 + C1   // loop-invariant addend
    acc2 = acc2 + C2
    counter = counter + step
}
```

And replaces them with O(1) closed-form assignments:

```c
if ((counter) < (N)) {
    acc1 = acc1 + (C1) * ((N) - counter);
    acc2 = acc2 + (C2) * ((N) - counter);
    counter = (N);
}
```

The guard prevents the collapsed form from running when the loop would not have executed at all (counter ≥ bound). The formula `(N - counter)` computes remaining trip count correctly for any initial counter value.

**What this handles that the C compiler cannot:**

| Loop type | Clang `-O2` | Aether collapse |
|-----------|------------|-----------------|
| `while i < 10 { total += 5; i++ }` | Collapses (constant bound, scalar evolution) | Collapses |
| `while i < n { total += 5; i++ }` | Cannot collapse (variable bound) | **Collapses** |
| Multi-accumulator `while i < n { a += 3; b += 7; i++ }` | Cannot collapse | **Collapses** |

For constant-bound loops the Aether collapse is redundant with clang's scalar evolution pass, so both emit equivalent O(1) code. The unique value is for *variable-bound* loops, which clang cannot analyze without the semantics that Aether's type system provides (no aliasing, no pointers, no side effects in the body).

**Detection requirements (all must hold):**
1. Condition is `var < bound` or `var <= bound`
2. Every body statement is `target = target + expr` (no other forms)
3. One statement increments the counter variable by a positive literal step
4. All addend expressions are loop-invariant (no reference to counter or other modified variables)
5. Bound expression is not modified by any loop body statement
6. No function calls or actor sends in the loop body (sends get batch-send treatment instead)

**Pure counter elimination** is a subcase handled automatically (zero accumulators):

```aether,fragment
i = 0
while i < n { i = i + 1 }   // n can be a runtime variable
```
→ `if ((i) < (n)) { i = (n); }` clang can collapse only when `n` is a compile-time constant.

**Reported in optimization stats:**
```
Optimization Statistics:
  Series loops collapsed: 3
```

---

### Linear Counter Sum (`compiler/codegen/codegen_stmt.c`)

Detects loops where an accumulator adds the **counter variable itself** (or a scaled version of it):

```aether,fragment
j = 0
total = 0
while j < N {
    total = total + j     // addend IS the induction variable
    j = j + 1
}
```

Replaced with the **triangular-number closed form**:

```c
if ((j) < (N)) {
    total = total + ((N) * ((N) - 1) / 2 - j * (j - 1) / 2);
    j = (N);
}
```

This is the sum Σ(i = j₀ to N−1) i = N(N−1)/2 − j₀(j₀−1)/2.

Also handles a scaled counter: `total = total + j * 3` → scale factor applied to the formula.

**Why clang cannot do this:** LLVM's Scalar Evolution (SCEV) identifies induction variables but does not synthesize the triangular-number closed form in the code generator. Polly (an optional LLVM extension, rarely enabled) can do affine loop transformations but is not part of the standard `-O2` pipeline.

| Loop type | Clang `-O2` | Aether collapse |
|-----------|------------|-----------------|
| `while j < n { total += 5; j++ }` | Cannot collapse (variable bound) | Series collapse |
| `while j < n { total += j; j++ }` | Cannot collapse (addend = counter) | **Linear collapse** |
| `while j < n { total += j * 3; j++ }` | Cannot collapse | **Linear collapse (scaled)** |
| Mixed: `while j < n { a += 5; b += j; j++ }` | Cannot collapse | **Both in one pass** |

**Reported in optimization stats:**
```
Optimization Statistics:
  Linear loops collapsed: 3
```

### Cooperative Preemption (Opt-In)

**Implementation:** `runtime/scheduler/multicore_scheduler.c`, `compiler/codegen/codegen_stmt.c`

Two independent mechanisms, both zero-cost when disabled:

**Scheduler-side** (`AETHER_PREEMPT=1`): After each `actor->step()` in the drain loop, check elapsed time. If the batch exceeds the threshold (default 1ms), break and move to the next actor.

```c
uint64_t _drain_start = preempt_enabled ? aether_now_ns() : 0;
while (...) {
    actor->step(actor);
    if (_drain_start && (aether_now_ns() - _drain_start) >= threshold_ns)
        break;
}
```

**Codegen-side** (`aetherc --preempt`): Insert `sched_yield()` at loop back-edges. Reduction counter limits yield frequency to every 10000 iterations.

```c
// Generated at top of every while/for body when --preempt is active
if (--_aether_reductions <= 0) { _aether_reductions = 10000; sched_yield(); }
```

### Actor Timeouts

**Implementation:** `compiler/codegen/codegen_actor.c`, `runtime/scheduler/multicore_scheduler.h`

`ActorBase` carries `timeout_ns` and `last_activity_ns`. The generated step function checks the nanosecond clock before `mailbox_receive()`. The scheduler polls timeout-active actors even when their mailbox is empty.

## References

- Lock-Free Programming: Harris, "A Pragmatic Implementation of Non-Blocking Linked-Lists"
- Cache Coherency: Intel 64 and IA-32 Architectures Optimization Reference Manual
- Memory Ordering: C11 Atomic Operations and Memory Model
- Actor Model: Hewitt, Bishop, Steiger (1973)
