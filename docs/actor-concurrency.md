# Aether Runtime Guide

Understanding the actor runtime, concurrency model, and message passing system.

## Performance Optimizations

The actor runtime includes several performance optimizations applied automatically. See [runtime-optimizations.md](runtime-optimizations.md) for detailed descriptions of each technique.

### Scheduler Design

**Partitioned Multicore Scheduler:**
- Locality-aware actor placement at spawn time (actors placed on the caller's core)
- Each core processes only its local actors in the fast path
- Work stealing activated when cores become idle
- Non-blocking work stealing using try-lock with ascending core-id lock ordering
- NUMA-aware allocation when available (Linux and Windows; on macOS `aether_numa_alloc` is a plain `malloc`)
- Message-driven actor migration co-locates communicating actors on the sender's core

**Work Stealing:**
- Triggered after extended idle scheduler cycles
- Selects the core with the most pending work
- Steals entire actors (not individual messages) to preserve cache locality
- Uses ascending core-id lock ordering consistent with migration to prevent deadlock

**Message Delivery:**
- Same-core messages: direct mailbox or SPSC queue write (no queue overhead)
- Cross-core messages: lock-free queue with batch dequeue
- Adaptive batch processing: 64 to 1024 messages per scheduler cycle

### Active Optimizations

**Main Thread Actor Mode:**
- Single-actor programs bypass the scheduler entirely
- Messages processed synchronously in the sender's context
- Zero-copy: caller's stack pointer passed directly
- Activated automatically when only one actor exists
- Disabled by `AETHER_NO_INLINE=1` environment variable

**Message Coalescing:**
- Batch dequeue drains multiple messages in a single atomic operation
- Configurable threshold (COALESCE_THRESHOLD = 512)

**Thread-Local Message Pools:**
- Per-thread pool of 256 pre-allocated buffers (up to 256 bytes each)
- Eliminates malloc/free on the message hot path
- Falls back to malloc for oversized messages or pool exhaustion

**Direct Send:**
- Bypasses incoming queue for same-core actors
- Writes directly to actor mailbox or SPSC queue
- Guarded by `current_core_id` to prevent data races from non-scheduler threads

**Adaptive Batching:**
- Dynamically adjusts batch size based on queue depth
- Increases under sustained load, decreases when idle

**Inline Single-Int Messages:**
- Messages with one integer field bypass pool allocation entirely
- Value encoded directly in `Message.payload_int`

## Actor Model

Aether implements a lightweight actor model where actors are state machines that communicate via asynchronous messages.

### Actor Lifecycle

1. **Spawn** - Create actor instance with `spawn(ActorName())`
2. **Send** - Send messages with the `!` operator or generated send functions
3. **Step** - Scheduler calls the actor's step function to process mailbox messages
4. **Deactivate** - Actor is marked inactive when its mailbox is empty

### Actor Structure

Each actor is a C struct. The first fields must match `ActorBase` layout:

```c
typedef struct {
    atomic_int active;
    int id;
    Mailbox mailbox;
    void (*step)(void*);
    pthread_t thread;
    int auto_process;
    atomic_int assigned_core;
    atomic_int migrate_to;           // Affinity hint: core to migrate to (-1 = none)
    atomic_int main_thread_only;     // If set, scheduler threads skip this actor
    SPSCQueue* spsc_queue;           // Lock-free same-core messaging (lazy alloc)
    _Atomic(ActorReplySlot*) reply_slot;  // Non-NULL during ask/reply
    atomic_flag step_lock;           // Prevents concurrent step() during work-steal
    uint64_t timeout_ns;             // Receive timeout in ns (0 = none)
    uint64_t last_activity_ns;       // Timestamp when idle started (0 = not idle)
    atomic_int dead;                 // 1 once step() unwound via panic; actor skipped after
} ActorBase;
```

User-defined actors extend this layout with additional fields after `dead`.

## Message Passing

### Mailbox

Each actor has a ring buffer mailbox with 32 slots (power-of-2 for fast masking):

```c
#define MAILBOX_SIZE 32
#define MAILBOX_MASK (MAILBOX_SIZE - 1)

typedef struct {
    Message messages[MAILBOX_SIZE];
    int head;
    int tail;
    _Atomic int count;
} Mailbox;
```

The mailbox is a single-producer single-consumer (SPSC) queue. The owning scheduler thread delivers messages and calls the actor's step function. `count` is `_Atomic` to handle the rare case where work-stealing transfers an actor between cores mid-flight. Cross-core messages arrive via the scheduler's per-sender lock-free incoming queues and are delivered to the mailbox by the owning core.

### Message Structure

```c
typedef struct {
    int type;           // Message type ID
    int sender_id;      // Sender actor ID
    intptr_t payload_int; // Integer payload (intptr_t to avoid truncation of actor refs on 64-bit)
    void* payload_ptr;  // Pointer to message data (pool or malloc allocated)
    struct {
        void* data;
        int size;
        int owned;
    } zerocopy;
    void* _reply_slot;  // ActorReplySlot* for ask messages, NULL for fire-and-forget
} Message;
```

### Sending Messages

Messages are sent asynchronously. The generated code routes messages through the scheduler:

```c
// Same-core: direct delivery
if (current_core_id >= 0 && current_core_id == atomic_load_explicit(&actor->assigned_core, memory_order_relaxed)) {
    scheduler_send_local(actor, msg);
} else {
    scheduler_send_remote(actor, msg, current_core_id);
}
```

For single-int messages, the code generator emits an inline path that stores the value directly in `msg.payload_int`, bypassing pool allocation.

### Receiving Messages

The `receive` block in the actor definition becomes the step function. The code generator emits a computed goto dispatch table for message handler selection:

```c
void ActorName_step(ActorName* self) {
    Message msg;
    if (!mailbox_receive(&self->base.mailbox, &msg)) {
        self->base.active = 0;
        return;
    }
    void* _msg_data = msg.payload_ptr;
    int _msg_id = msg.type;

    static void* dispatch_table[256] = {
        [1] = &&handle_MessageA,
        [2] = &&handle_MessageB,
    };

    if (_msg_id >= 0 && _msg_id < 256 && dispatch_table[_msg_id]) {
        goto *dispatch_table[_msg_id];
    }
    return;

    handle_MessageA:
        // process message
        return;
}
```

## Actor Timeouts

The `after` clause on a `receive` block fires a handler if no message arrives within a given number of milliseconds:

```aether
actor Monitor {
    receive {
        Heartbeat -> { println("alive") }
    } after 5000 -> {
        println("no heartbeat for 5s")
    }
}
```

The timeout is one-shot: it is cancelled when any message is received. The countdown starts when the actor's mailbox becomes empty. Internally, the generated step function checks `_aether_clock_ns()` against a deadline before each `mailbox_receive()` call.

The timeout expression can reference actor state fields, not just integer literals, useful when the interval is configurable per-actor or computed:

```aether
actor Poller {
    state interval_ms = 30000
    state ticks       = 0

    receive {
        Tick -> { ticks = ticks + 1 }
    } after interval_ms -> {
        self ! Tick {}
    }
}
```

Identifiers in the timeout **expression** resolve against actor state fields only. `self` is **not** in scope there: the timeout expression is generated at spawn time (in `spawn_<Actor>`, before `self` exists), so a state-field reference such as `after interval_ms` resolves against the freshly-allocated actor handle. The timeout **body** (above) runs later inside the step function, where `self` *is* in scope, `self ! Tick {}` is fine.

## Panic and Stack Traces

`panic("reason")` unwinds via `aether_panic`. If a `try { ... } catch e { ... }` block (or the scheduler's per-step barrier for actor handlers) is on the stack, the panic is caught and `e` binds to the reason. If no frame catches, the runtime prints the reason to stderr along with a filtered stack trace, then aborts:

```text
aether: panic outside any try/catch or actor: divide by zero

Stack trace (most recent call first):
  0: math.safe_divide
  1: calculate_value
  2: main
```

The trace is captured at the call site (codegen wires `aether_panic_capture_stack` in front of every `aether_panic` call) so the user's caller frames survive `-O2` tail-call optimisation. Symbols starting with `aether_` are pretty-printed back to their dotted Aether names, `aether_std_string_concat` reads as `std.string.concat`. User-code symbols pass through verbatim.

The trace is **best-effort diagnostic info**. Under `-O2`, function inlining can fuse callers into one frame; a panic deep inside a chain of small helpers may show only `main`. For richer development-time traces, build with `ae build --quick`, which compiles at `-O0 -g` instead of the default `-O2`; the per-call frames return.

To suppress the trace block entirely (useful for tests that diff stderr line-for-line), set `AETHER_STACK_TRACE=0` in the environment. The reason line still prints.

Available on glibc-Linux and macOS today (uses `<execinfo.h>` `backtrace()` from libc proper, no extra link dependency). On musl, Windows, Emscripten, and freestanding targets the trace block is skipped, `panic`/`try`/`catch` themselves still work.

### Heap cleanup on unwind

A caught panic does not leak the allocations made since the `try`. While
any panic frame is live (a `try`/`catch` block, or the scheduler's
per-step barrier), generated code journals every heap allocation whose
deferred scope-exit free is armed; `aether_panic()` drains the innermost
frame's still-live entries before the jump, freeing exactly the deferred
frees the `longjmp` would have skipped. The journal mirrors the deferred
frees one-for-one:

- A value freed normally (explicit `string.release`, reassignment, scope
  exit) leaves the journal at the same moment, so the drain never
  double-frees.
- A value that escaped the guarded block (returned, stored in a map,
  list, actor state, or message) is never journaled or is forgotten at
  the handoff, so the drain never frees something another owner holds.
- Nested `try` drains only the inner frame's entries; the outer frame's
  allocations survive to their normal frees.

For actor handlers this means a panicking actor's step-scoped
allocations are reclaimed before the actor is marked dead. The journal
is scoped to guarded regions: outside any frame the hooks are a
TLS-load-and-return, and the no-panic hot path shows no measurable cost
on the ping-pong messaging benchmark. Regression coverage:
`tests/regression/test_panic_unwind_cleanup.ae` (the full matrix),
`tests/integration/panic_unwind_no_leak/` (50000 caught panics, RSS
flat), `tests/integration/panic_actor_step_drain/` (scheduler barrier).

Main-thread inline mode (single-actor programs) has no step barrier by
design, so a handler panic there still reaches the no-frame fallback
(reason + trace + abort), unchanged.

## Cooperative Preemption

By default, message handlers run to completion. A tight compute loop inside a handler will block that core's scheduler thread. For programs where this is a concern, cooperative preemption can be enabled:

- **Scheduler-side**: `AETHER_PREEMPT=1` enables time-based drain loop breaks. After each `actor->step()` call, the scheduler checks elapsed time and yields if the threshold (default 1ms, configurable via `AETHER_PREEMPT_MS`) is exceeded. Cost when disabled: zero.
- **Codegen-side**: `aetherc --preempt` inserts `sched_yield()` calls at loop back-edges in generated C code. A reduction counter (10000 iterations) triggers the yield. Cost when not compiled with `--preempt`: zero (code not generated).

Both levels are independent and can be used alone or together.

## Single-Core Runtime

In single-core mode, actors run cooperatively. The scheduler processes actors in a loop, calling each actor's step function when its mailbox contains messages.

## Multi-Core Runtime

The multi-core scheduler uses core partitioning with locality-aware placement:

- N cores = N independent scheduler threads
- Each scheduler runs in a pthread pinned to a core
- Actors placed on the caller's core at spawn time (main thread defaults to core 0; scheduler threads use their own core). This keeps parent-child actor groups co-located for efficient local messaging.
- Cross-core messages use lock-free queues with batch dequeue/enqueue
- Message-driven migration converges communicating actors onto the same core

### Synchronization

The `wait_for_idle()` function blocks until all actors have finished processing their messages (quiescence). It is **non-destructive**: it does not stop or join scheduler threads, so it can be called multiple times in a program. This is the recommended way to synchronize the main thread with actor completion:

```aether
main() {
    ping = spawn(PingActor())
    pong = spawn(PongActor())

    // Send initial message
    ping ! Start {}

    // Wait for all messages to be processed
    wait_for_idle()

    // Actors are now idle, safe to read state
    print(ping.result)

    // Can send more messages and wait again
    ping ! Start {}
    wait_for_idle()
}
```

The implementation uses per-core message counters to detect idle state with minimal overhead on the message-passing hot path. Each scheduler core tracks messages sent and processed locally. The `wait_for_idle()` builtin lowers to the C function `scheduler_wait()`, which sums across cores to determine when all in-flight messages have been handled.

### Initialization

```c
int main() {
    scheduler_init(4);  // Use 4 cores
    scheduler_start();

    // Create and use actors

    scheduler_shutdown();  // Waits for quiescence, stops threads, joins them
}
```

**Scheduler lifecycle functions:**
- `scheduler_wait()` -- waits for quiescence (all pending messages processed). Non-destructive: scheduler threads keep running. Safe to call multiple times.
- `scheduler_shutdown()` -- waits for quiescence, then stops scheduler threads and joins them. Called once at program exit.

### Core Assignment and Migration

Actors are placed on the caller's core at spawn time. Actors spawned from the main thread default to core 0, keeping top-level actor groups co-located. Actors spawned from within actor handlers inherit the parent's core.

During operation, cross-core message senders set a `migrate_to` hint on the target actor, requesting migration to the sender's core. The owning scheduler thread checks these hints after processing messages in the coalesce buffer and migrates the actor accordingly. This co-locates communicating actors regardless of their initial placement. Migration is non-blocking: if the destination lock cannot be acquired, migration is deferred to the next iteration.

### Message Routing

- **Same-core messages:** Direct mailbox or SPSC queue write (verified via `current_core_id`)
- **Cross-core messages:** Enqueued to target core's lock-free incoming queue
- **Migrated actor messages:** Forwarded to the actor's current core with spin-retry

## Memory Management

Actor memory is allocated using NUMA-aware allocation (`aether_numa_alloc`) at spawn time. The `scheduler_spawn_actor` function accepts the full derived-struct size to ensure correct allocation for user-defined actor types.

Message payloads are managed by thread-local pools. Payloads are returned to the pool on free via `aether_free_message`, which checks whether the pointer falls within the pool range before falling back to `free`.

## Memory Model

The actor runtime uses several allocation strategies for automatic cleanup:
- **Actor memory**: one NUMA-aware allocation per actor at spawn time (`aether_numa_alloc`), freed with `aether_numa_free` when the actor is destroyed. The mailbox is an inline struct member, not a separate allocation.
- **Message payloads**: thread-local pools with automatic return
- **Arenas**: a general-purpose arena allocator provides bulk deallocation without per-object tracking for opt-in uses (`std.arena`, JSON parsing). The actor path does not use arenas.

See [Memory Management](memory-management.md) for details on arenas, pools, and allocation strategies.

## Patterns

- **Per-process configuration**, Aether rejects mutable assignment to module-level identifiers, so the C `static char *g_config` shape doesn't transcribe. The canonical replacement is the actor-singleton pattern: spawn one config actor at startup, drive writes via `Set*` messages, drive reads via `Get*` messages that carry an `actor_ref` reply target. See [per-process-config.md](per-process-config.md) for the worked example, the trade-offs (when message round-trip is acceptable vs. when to plumb the value as a parameter or worker-actor cache), and a port of a real C global-state shim.
