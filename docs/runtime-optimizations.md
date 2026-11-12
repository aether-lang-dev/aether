# Runtime Optimizations

## Overview

The Aether runtime implements several performance optimizations targeting message-passing overhead, synchronization primitives, and memory access patterns. Each optimization is integrated into the scheduler and message delivery paths.

The optimizations are tiered by how they activate:

- **Tier 0, platform capabilities (compile-time):** `AETHER_HAS_THREADS`, `AETHER_HAS_ATOMICS`, `AETHER_HAS_FILESYSTEM`, `AETHER_HAS_NETWORKING`, `AETHER_HAS_SIMD`, `AETHER_HAS_NUMA`, `AETHER_HAS_AFFINITY`. Auto-detected from the target; any flag can be forced off with `-DAETHER_NO_<FEATURE>` (e.g. `-DAETHER_NO_THREADING`). See [Platform Detection](architecture.md#platform-detection-tier-0).
- **Tier 1, always on:** the Active Optimizations below (direct send, adaptive batching, message coalescing).
- **Tier 2, auto-detected at startup:** SIMD batch processing (AVX2/NEON), MWAIT idle (x86 MONITOR/MWAIT), CPU core pinning (OS-dependent).
- **Tier 3, opt-in:** lock-free mailboxes and message deduplication, see [Opt-In Features](#opt-in-features).

## Active Optimizations

### Every send goes through the scheduler

**Implementation:** `runtime/actors/aether_send_message.c`, `runtime/scheduler/multicore_scheduler.c`

A send (`!`) is asynchronous whatever the number of actors: the payload is
copied to the heap and handed to the scheduler, and the sender goes on while a
scheduler thread runs the handler. The runtime once ran the first actor's
handler inside the sender's `!` until a second actor was spawned ("main-thread
mode"), so a handler that blocked, waiting on something its sender was about
to do, deadlocked a one-actor program, and the order of a program's output
depended on how many actors it had (#3112). The cooperative (no-threads)
runtime queues a send the same way and runs it at the next `wait_for_idle`,
`aether_scheduler_poll` or shutdown.

### Message Payloads

**Implementation:** `runtime/actors/aether_send_message.c`

A typed message whose struct fits the bytes `Message` has from `payload_int` up to `zerocopy.owned` (28 on 64-bit targets, 16 on 32-bit) travels in the message itself, with `zerocopy.owned` set to `AETHER_MSG_INLINE` (#3534): two ints, an int and a string, a reference and a string. The generated step copies it to the actor's `_aether_step_inline` and runs the handler there, so such a send makes no allocation and its handling frees none. Any other typed message's payload is a heap copy, made by the send and freed with `free()` by whoever handles or drops the message (`aether_free_message`). There is no payload pool (#3374): a per-thread pool was tried, but an actor can move to another core after a message is sent, so a pooled payload would be released by a thread that does not own the pool. `AETHER_FLAG_LOCKFREE_POOLS` is accepted and ignored, and `aether_message_pool_stats` reports zeros; both are deprecated.

### Message Coalescing with Batch Dequeue

**Implementation:** `runtime/scheduler/multicore_scheduler.c`, `runtime/scheduler/lockfree_queue.h`

The scheduler drains multiple messages from the lock-free incoming queue in a single batch dequeue operation. This reduces atomic operations from one-per-message to one-per-batch.

```c
#define SCHEDULER_DRAIN_BATCH 512

// Single batch dequeue: 1 atomic store for entire batch
count = queue_dequeue_batch(&incoming_queue, buffer.actors, buffer.messages, batch_size);

for (int i = 0; i < count; i++) {
    // Redirect migrated actors, deliver to mailbox, or enqueue to SPSC
}
```

The batch dequeue reads `head` and `tail` once, copies all available messages, then advances `head` with a single `atomic_store`. This matches the existing batch enqueue pattern used by `queue_enqueue_batch`.

### Batch Send for Fan-Out Patterns

**Implementation:** `runtime/scheduler/multicore_scheduler.c`, `compiler/codegen/codegen.c`

For main thread fan-out patterns (fork-join), batch send reduces atomic operations from N to num_cores. The compiler wraps a while loop of a non-actor function in batch start/flush calls when the loop sends and cannot wait between its sends (#3117): it calls nothing, prints nothing, asks nothing and reads no actor's state, and has no `return`, labeled `break` or `continue` (which can leave for an enclosing loop) or closure in it. Sends, arithmetic, assignments, branches and inner loops are fine, the counting and fork-join shape. A loop that sleeps, reads a line or waits between sends is not batched, since its messages would wait for the loop's end.

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

**Pattern detection:** Only applied outside actor receive handlers, which preserves low-latency actor-to-actor messaging while optimizing main-to-actors fan-out. Inside a batch every send joins it, a message with a payload included (`aether_send_message_batch`), so the loop's sends to one actor arrive in the order they were made; an inner loop joins the outer loop's batch. A send that goes around a pending batch flushes it first, and so does `wait_for_idle()`; opening a batch never drops what one holds. On a scheduler thread nothing is batched: its sends keep their core's own channel and overflow order.

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

### Sender-Side Batching (the send buffer)

**Implementation:** `runtime/actors/aether_send_buffer.c`, `scheduler_send_many` in `runtime/scheduler/multicore_scheduler.c`

`send_buffered(actor, msg)` gathers a thread's messages to one actor in a thread-local buffer of `SEND_BUFFER_SIZE` (256), and a flush hands them to `scheduler_send_many`, which sends them as that many single sends would go: counted as sent, behind whatever is queued for the actor, and kept when a queue is full. To an actor of another core, from a core's thread, they are enqueued in one batch (one atomic update), what the queue has no room for waiting in the sender's deferred buffer in order. A flush always empties the buffer (#3292). Same-core messages used to go to a per-actor SPSC queue (`runtime/actors/aether_spsc_queue.h`, `ActorBase.spsc_queue`), outside every ordering rule; nothing fills it now. The buffer is a `static` thread-local of `aether_send_buffer.c`; the inline functions of `aether_send_buffer.h` reach it through `send_buffer_here()`, as every runtime thread-local of more than one field is reached from outside its file (`runtime/actors/aether_frames.h` says why).

### Direct Mailbox Delivery

**Implementation:** `runtime/scheduler/multicore_scheduler.c` (`scheduler_send_local`, `send_through_core`)

When the sender is on the same core as the target actor and is actually running on that core's scheduler thread (verified via `current_core_id`), the message bypasses the incoming queue and goes straight into the actor's mailbox, unless a message of the actor is still in the queues (#3093) or in its overflow (#3241), which it then goes behind. An actor with a thread of its own takes every message in its inbox instead (#2598).

The `current_core_id` guard prevents non-scheduler threads (such as the main thread) from writing to the mailbox concurrently with the scheduler thread, which would be a data race on the non-thread-safe ring buffer.

### Message-Driven Actor Migration

**Implementation:** `runtime/scheduler/multicore_scheduler.c` (`scheduler_send_remote`, scheduler actor loop)

When a cross-core send occurs, the sender sets a `migrate_to` hint on the target actor, requesting migration to the sender's core. The scheduler thread that owns the actor checks migration hints in two places:

1. **After coalesce-buffer processing** (targeted, O(batch_size)): Immediately after delivering and processing messages from the coalesce buffer, the scheduler checks `migrate_to` on each actor it just processed. This provides fast migration response without scanning the full actor list.

2. **From the ready list** (O(actors with work), #3540): a hint the targeted path could not act on (the move failed: a lock was taken, a message of the actor was in flight) pushes the actor on its core's ready list, and a work steal pushes the actor it asks for, so the core asks the move again on its next pass. The scheduler used to scan all local actors for pending hints whenever no message arrived through its queues, which loaded every actor of the core, idle ones included, on every idle pass.

Migration uses ascending core-id lock ordering to prevent deadlock between concurrent migration and work-stealing operations. Both locks are acquired via non-blocking try-lock, if either lock is contended, migration is deferred to the next iteration. The actor is always processed before migration is attempted, ensuring progress even under constant migration pressure.

### Inline Single-Field Messages

**Implementation:** `compiler/codegen/codegen.c`

The code generator detects messages with exactly one scalar field that fits in `intptr_t` and emits an inline fast path. Instead of heap-allocating the message payload (malloc + memcpy + free), the field value is stored directly in `Message.payload_int`. The receiver reconstructs the struct on the stack. This eliminates all heap allocation for single-field messages.

**Supported field types** (from `is_inlineable_scalar`): `int`, `int64`, `uint64`, `duration`, and `ptr`. Other types, including `bool` and `actor_ref`, are not inlined and take the heap-allocated payload path.

**Impact:** Eliminates malloc/free for single-field messages, which are the most common pattern in tree-spawn and request-response workloads. Run `make benchmark` to measure the effect on your hardware.

### Computed Goto Dispatch

**Implementation:** `compiler/codegen/codegen_actor.c` (generated code)

The code generator emits a `dispatch_table[256]` with GCC computed goto (`goto *dispatch_table[_msg_id]`) for message handler selection. This replaces indirect function calls or switch statements with direct label jumps. The message ID is read from `msg.type` (`int _msg_id = msg.type;`) rather than dereferencing the payload pointer.

The same lowering applies to interpreter loops written in Aether (#2378;
`compiler/codegen/codegen_stmt.c`, `td_loop_switch`): a `while true { head;
switch sel { ... continue ... } }` loop gets a static label table per loop, and
each `continue` that targets it repeats the head and jumps with `goto
*_ae_td_tbl_N[sel]`. A value with no entry jumps back to the plain `switch`, so
the table is never needed for correctness; under MSVC or
`-DAETHER_NO_THREADED_DISPATCH` the loop compiles as it did before. In an
`--emit=lib` build the call-deadline check that sits at the loop head is
repeated at every threaded dispatch. See the language reference, *Interpreter
dispatch loops*, for the recognised shape.

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
```

### Relaxed Atomic Memory Ordering

**Implementation:** `runtime/scheduler/multicore_scheduler.c`

Non-critical atomic operations use `memory_order_relaxed` to avoid unnecessary memory barriers:

```c
// Scheduler idle tracking
atomic_fetch_add_explicit(&sched->idle_cycles, 1, memory_order_relaxed);
atomic_store_explicit(&sched->idle_cycles, 0, memory_order_relaxed);
```

Approximate work counts do not require sequential consistency. Relaxed ordering provides atomicity without the overhead of full memory barriers.

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

The `cpu_recommend_cores` function uses CPUID on x86 and falls back to OS-level APIs (`sysctl` on macOS, `sysconf` on Linux, `GetNativeSystemInfo` on Windows) when CPUID returns zero (ARM, virtualized environments). It then takes no more threads than the CPUs the process may use, `aether_cpu_available()`: the affinity mask (`taskset`, a cpuset cgroup, a Windows job or `start /affinity`) and, on Linux, a cgroup CPU quota (`docker --cpus`, a Kubernetes limit). The cap of 16 still applies. The machine's count started 16 scheduler threads for a 2-CPU container, which then preempted each other (#3106). `std.worker` and the HTTP server size their pools the same way.

### Apple Silicon P-Core Detection

**Implementation:** `runtime/utils/aether_cpu_detect.c`, `runtime/scheduler/multicore_scheduler.c`

On Apple Silicon (M1/M2/M3), the runtime detects and uses only Performance cores (P-cores) to ensure consistent throughput. Efficiency cores (E-cores) are excluded because they run at lower clock speeds and can cause variance in benchmarks.

Detection uses `sysctlbyname("hw.perflevel0.physicalcpu")` to query the P-core count. Combined with `QOS_CLASS_USER_INTERACTIVE` thread priority, this encourages macOS to schedule actor threads on P-cores.

**Platform-specific thread affinity:**

| Platform | Mechanism | Binding |
|----------|-----------|---------|
| Linux | `pthread_setaffinity_np` | Hard binding |
| macOS | `thread_policy_set` + QoS | Hint only |
| Windows | `SetThreadIdealProcessor` | Hint only |

Note: macOS does not support hard core pinning by design, and Windows uses a hint by choice: bound to one logical CPU, a scheduler thread could not leave it when another program ran there, and shared it a time slice at a time (#3087). The OS keeps a thread on its CPU and moves it when that CPU is busy.

Linux keeps the hard binding, measured (#3089). On a 4-CPU CI runner, bound and unbound threads were compared under busy loops:
- bound won 7 of the 12 loaded cases (thread ring 10.8 against 7.4 M msg/s beside two loops the kernel placed, skynet up to 2.4x);
- bound lost only where another program's thread was pinned to a scheduler thread's own CPU, by 1.1x to 1.5x;
- on a quiet machine they tied.

Linux moves movable load off a bound thread's CPU, which Windows did not.

On Linux and Windows, scheduler thread i is placed on the i-th CPU the process may run on, counted from its affinity mask when the threads start, not on CPU i (#3106). Under `taskset -c 4-7` the threads stay on CPUs 4 to 7, where binding to CPU 0 escaped the mask. In a cpuset cgroup no binding is refused for a CPU outside the set.

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

**Dead branch in generated send code** (`compiler/codegen/codegen_expr.c`)

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
while counter < N {          // or <=
    acc1 = acc1 + C1         // loop-invariant addend
    acc2 = acc2 + counter    // or counter * K: a linear sum
    counter = counter + step // a positive integer literal
}
```

and replaces them with the closed form. With `c0` the counter on entry and `T` the trip count, `ceil((N - c0) / step)` for `<` and `floor((N - c0) / step) + 1` for `<=`:

```c
if ((counter) < (N)) {
    /* T computed from the distance N - c0 and the step */
    acc1 = acc1 + C1 * T;
    acc2 = acc2 + (T*c0 + step * T*(T-1)/2);   /* T*(T+1)/2 if acc2's statement follows the increment */
    counter = c0 + step * T;
}
```

The guard keeps everything unchanged when the loop would not run at all. `N` can be any side-effect-free expression, runtime values included, and the counter can start anywhere.

**Only integer loops are collapsed.** The counter, the bound, every accumulator and every addend must be `int` or `long`. Those wrap (`-fwrapv`), and the closed form is computed in `uint64_t`, where each step is exact modulo 2^64, then narrowed to each variable's type, so the result is the loop's to the bit, wrapped accumulators included. A float series is left as a loop: repeated addition rounds at every step and a product does not, so no closed form gives the same answer. Before #2271 a float loop went through `(int64_t)` and ended at its bound (`while a < 0.0 { a = a + 6.28 }` from -0.51 left `0.0`), and an integer step that did not divide the distance stopped at the bound as well.

**The one wrap the closed form cannot follow** is the counter's own last step: when `c0 + step*T` passes the counter type's maximum, the loop wraps round and carries on. That is checked at run time, and the loop then runs as written.

**What this handles that the C compiler cannot:**

| Loop type | Clang `-O2` | Aether collapse |
|-----------|------------|-----------------|
| `while i < 10 { total += 5; i++ }` | Collapses (constant bound, scalar evolution) | Collapses |
| `while i < n { total += 5; i++ }` | Cannot collapse (variable bound) | **Collapses** |
| Multi-accumulator `while i < n { a += 3; b += 7; i++ }` | Cannot collapse | **Collapses** |

For constant-bound loops the Aether collapse is redundant with clang's scalar evolution pass, so both emit equivalent O(1) code. The unique value is for *variable-bound* loops, which clang cannot analyze without the semantics that Aether's type system provides (no aliasing, no pointers, no side effects in the body).

**Detection requirements (all must hold):**
1. Condition is `var < bound` or `var <= bound`, both `int` or `long`
2. Every body statement is `target = target + expr` (no other forms)
3. Exactly one statement increments the counter, by a positive integer literal
4. Every accumulator and addend is `int` or `long`
5. Invariant addends do not reference the counter or any other modified variable
6. Bound expression is not modified by any loop body statement
7. No function calls or actor sends in the loop body (sends get batch-send treatment instead)

**Pure counter elimination** is a subcase handled automatically (zero accumulators):

```aether,fragment
i = 0
while i < n { i = i + 1 }   // n can be a runtime variable
```
→ `i` becomes `i + T` without a loop; clang can collapse this only when `n` is a compile-time constant.

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

Replaced with the closed form of the sum of the values the statement sees:

```c
if ((j) < (N)) {
    total = total + (T*j0 + step * T*(T-1)/2);
    j = j0 + step * T;
}
```

That is Σ(k = 0 … T−1) (j₀ + k·step). When the accumulator's statement comes after the increment it sees the stepped value, and the sum runs k = 1 … T, so `T*(T+1)/2`. Any positive integer step works. `T*(T±1)/2` halves whichever factor is even, so it stays exact modulo 2^64 without a 128-bit product.

Also handles a scaled counter: `total = total + j * 3` multiplies the sum by 3. The loop computes `j * 3` in the product's own type, so a scaled sum is collapsed only when that product is at least as wide as the accumulator (an `int` product feeding a `long` accumulator could wrap where the sum does not).

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

`ActorBase` carries `timeout_ns` and `last_activity_ns`. The generated step function checks the nanosecond clock before `mailbox_receive()`. Each core keeps its actors' armed countdowns in a min-heap of deadlines (#3540) and steps an actor whose deadline has passed, with no message to step it for; it parks no longer than until the earliest deadline.

## References

- Lock-Free Programming: Harris, "A Pragmatic Implementation of Non-Blocking Linked-Lists"
- Cache Coherency: Intel 64 and IA-32 Architectures Optimization Reference Manual
- Memory Ordering: C11 Atomic Operations and Memory Model
- Actor Model: Hewitt, Bishop, Steiger (1973)
