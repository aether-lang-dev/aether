# Aether Scheduler - Quick Reference

## Main Thread Mode

Single-actor programs bypass the scheduler entirely. When only one actor exists and all messages originate from the main thread, the runtime processes messages synchronously without spawning scheduler threads.

**Activation:**
- Automatic: Enabled when the first actor spawns, disabled when a second actor spawns
- Manual disable: Set `AETHER_NO_INLINE=1` environment variable

**Mechanism:**
- `scheduler_start()` returns immediately
- `scheduler_wait()` returns immediately (non-destructive; does not stop threads)
- `aether_send_message` processes synchronously via `aether_send_message_sync`
- **Threaded mode:** Zero-copy, message data passed directly from caller's stack
- **Cooperative mode:** Heap-copy, message data is `malloc`'d because mailbox FIFO order means the sent message may not be consumed by the immediate `step()` call

**Per-actor flag:**
The `main_thread_only` field on `ActorBase` signals scheduler threads to skip processing this actor. When a second actor spawns, the flag is cleared on the first actor. In cooperative mode, this flag stays set for all actors.

---

## Cooperative Scheduler

On platforms without pthreads (WebAssembly, embedded) or when threading is explicitly disabled (`-DAETHER_NO_THREADING`), the cooperative scheduler (`aether_scheduler_coop.c`) provides the same API as the multi-core scheduler but runs everything on a single thread.

**Key differences:**
- `scheduler_init()` forces `num_cores = 1`, `main_thread_mode = true`
- `scheduler_start()` / `scheduler_stop()` are no-ops
- `scheduler_wait()` polls all actors via `aether_scheduler_poll()` until no messages remain
- All sends go to local mailbox (no cross-core routing)
- Ask/reply is synchronous (poll until `reply_ready`)

**Selection:** Controlled by `PLATFORM=wasm|embedded` or auto-detected from `EXTRA_CFLAGS` containing `AETHER_NO_THREADING`. Only one scheduler .c file is compiled.

```bash
# Test cooperative mode on native
make ci-coop

# Build for WASM via CLI
ae build --target wasm hello.ae

# Build for WASM via Makefile
make stdlib PLATFORM=wasm
```

---

## Actor Timeouts

Actors with a `receive ... after N` clause have `timeout_ns` and `last_activity_ns` fields in `ActorBase`. The generated step function checks the clock via `_aether_clock_ns()` before `mailbox_receive()`:

1. If the mailbox is empty and `timeout_ns > 0`, `last_activity_ns` is set to the current time (start countdown)
2. On the next poll, if `(now - last_activity_ns) >= timeout_ns`, the timeout handler fires and the timeout is disabled (one-shot)
3. If a message arrives, both `timeout_ns` and `last_activity_ns` are reset to 0 (cancelled)

Both the multicore and cooperative schedulers poll actors with active timeouts even when their mailbox is empty.

---

## Cooperative Preemption

Opt-in, zero cost when disabled. Two independent levels:

**Scheduler-side** (`AETHER_PREEMPT=1`): After each `actor->step()` call in the drain loop, the scheduler checks elapsed time via `aether_now_ns()`. If the batch exceeds the threshold (default 1ms, configurable via `AETHER_PREEMPT_MS`), it breaks out and moves to the next actor. Cost when disabled: zero (branch skipped).

**Codegen-side** (`aetherc --preempt`): Inserts `sched_yield()` at the top of every `while` and `for` loop body. A `static int _aether_reductions` counter (initialized to 10000) controls frequency, yields only when the counter hits zero. Note this counter is a single file-scope global, not thread-local, so in a multi-threaded actor program it is shared across all threads. Cost when `--preempt` is not passed: zero (code not generated).

---

## Scheduler Usage

```c
// Initialize with N cores (auto-detects hardware if 0)
scheduler_init(4);

// Register actors (assigned to preferred core)
scheduler_register_actor((ActorBase*)actor, preferred_core);

// Start processing
scheduler_start();

// Send messages (routed automatically)
for (int i = 0; i < 10000; i++) {
    Message msg = message_create_simple(1, 0, i);
    scheduler_send_remote(actor, msg, current_core_id);
}

// Wait for quiescence (non-destructive, can be called multiple times)
scheduler_wait();

// Can send more messages and wait again
for (int i = 0; i < 5000; i++) {
    Message msg = message_create_simple(1, 0, i);
    scheduler_send_remote(actor, msg, current_core_id);
}
scheduler_wait();

// At program exit: wait for quiescence, stop and join scheduler threads
scheduler_shutdown();
```

## Actor Allocation

```c
// NUMA-aware allocation with correct derived-struct size
ActorBase* actor = scheduler_spawn_actor(preferred_core, step_fn, sizeof(MyActor));
if (!actor) {
    // Fallback to manual allocation
    actor = aligned_alloc(64, sizeof(MyActor));
    memset(actor, 0, sizeof(MyActor));
    mailbox_init(&actor->mailbox);
    spsc_queue_init(&actor->spsc_queue);
}
```

## Message Flow

```
sender -> scheduler_send_remote -> lock-free queue -> batch dequeue
       -> coalesce buffer -> redirect/deliver -> mailbox -> actor step
```

**Same-core fast path:**
```
sender -> scheduler_send_local -> mailbox (direct) -> actor step
```

## Configuration

### Message Coalescing
```c
#define COALESCE_THRESHOLD 512  // In multicore_scheduler.h
```

### Adaptive Batch Size
```c
#define MIN_BATCH_SIZE 64    // In aether_adaptive_batch.h
#define MAX_BATCH_SIZE 1024
```

### Mailbox Size
```c
#define MAILBOX_SIZE 32   // In actor_state_machine.h (must be power of 2)
```

### Message Pool
```c
#define MSG_PAYLOAD_POOL_SIZE 256  // In aether_send_message.c
#define MSG_PAYLOAD_MAX_SIZE 256   // Max pooled message size
```

### Cross-Core Queue
```c
#define QUEUE_SIZE 1024  // In lockfree_queue.h
```

## Idle Detection

`scheduler_wait()` blocks until all messages have been processed (quiescence). It is **non-destructive**: scheduler threads continue running after it returns, so it can be called multiple times in a program (e.g., between test phases or message bursts). It uses per-core counters to detect idle state without atomic contention on the message-passing hot path:

`scheduler_shutdown()` calls `scheduler_wait()` internally, then stops scheduler threads and joins them. Call it once at program exit.

```c
// Per-core counters in Scheduler struct
uint64_t messages_sent;      // Incremented on send (no atomic)
uint64_t messages_processed; // Incremented on process (no atomic)

// scheduler_wait() sums across cores
while (total_sent != total_processed) {
    // spin-wait
}
```

Messages sent from the main thread (before scheduler threads start) use a separate atomic counter. This path is infrequent.

## Core Assignment and Migration

Actors are assigned to cores at spawn time using locality-aware placement: the runtime places each actor on the caller's core so that parent-child messaging stays local. Actors spawned from the main thread default to core 0, keeping all top-level actors co-located for efficient intra-group messaging. Actors spawned from within actor handlers (on scheduler threads) inherit the parent's core.

During operation, cross-core senders set a `migrate_to` hint on the target actor. The owning scheduler checks migration hints after processing messages in the coalesce buffer and migrates the actor to the sender's core. This co-locates communicating actors regardless of their initial placement.

Both migration and work stealing use ascending core-id lock ordering to prevent deadlock.

## Best Practices

**Do:**
- Let the scheduler handle core assignment and migration
- Use `scheduler_send_remote` for all cross-core messages
- Initialize `migrate_to = -1` when creating actors manually
- Ensure test actor structs match `ActorBase` field layout exactly

**Do not:**
- Access an actor's mailbox from a thread other than its owning scheduler thread
- Call `mailbox_send` directly on actors owned by other cores
- Omit `migrate_to` from custom actor structs (causes struct layout mismatch)

## Troubleshooting

### Low Throughput
- Verify compiler flags include `-O3 -march=native -flto`
- Check that message pool is active (pool stats via `aether_message_pool_stats`)
- Ensure batch size is appropriate for workload
- Verify core count matches available hardware

### High Latency
- Tune `COALESCE_THRESHOLD` for latency-sensitive workloads (lower = less batching delay)
- Check progressive backoff thresholds
- Avoid blocking operations in actor step functions

### Crashes
- Verify all actor structs match `ActorBase` layout (fields, in order: `active`, `id`, `mailbox`, `step`, `thread`, `auto_process`, `assigned_core`, `migrate_to`, `main_thread_only`, `spsc_queue`, `reply_slot`, `step_lock`, `timeout_ns`, `last_activity_ns`, `dead`)
- Initialize `migrate_to = -1` after actor creation
- Use `scheduler_send_remote` instead of direct mailbox writes for cross-core messages
- Call `scheduler_shutdown()` before process exit (waits for quiescence, stops and joins threads)

## Files Reference

### Headers
- `runtime/actors/actor_state_machine.h` - Message struct, mailbox
- `runtime/scheduler/multicore_scheduler.h` - Scheduler API, ActorBase, spinlock
- `runtime/scheduler/lockfree_queue.h` - Cross-core message queue
- `runtime/actors/aether_spsc_queue.h` - Same-core SPSC queue
- `runtime/actors/aether_adaptive_batch.h` - Adaptive batch sizing

### Implementation
- `runtime/scheduler/multicore_scheduler.c` - Multi-core scheduler: coalescing, migration, work stealing
- `runtime/scheduler/aether_scheduler_coop.c` - Cooperative single-threaded scheduler (WASM, embedded)
- `runtime/config/aether_optimization_config.h` - Platform detection (Tier 0 `AETHER_HAS_*` flags)
- `runtime/actors/aether_send_message.c` - Message sending, thread-local pools
- `runtime/actors/aether_actor_thread.c` - Actor-owned thread loop
- `runtime/utils/aether_thread.h` - Portable thread primitives (POSIX/Win32/no-op stubs)
- `runtime/aether_numa.c` - NUMA topology detection and allocation

### Tests
- `tests/runtime/test_scheduler.c` - Scheduler correctness tests
- `tests/runtime/test_scheduler_stress.c` - Concurrency stress tests
- `tests/runtime/test_scheduler_optimizations.c` - Optimization validation
