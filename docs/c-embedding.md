# Embedding Aether Actors in C Applications

This guide explains how to embed Aether actors in your existing C applications. There are two approaches: compiling Aether code to C and linking it, or using the runtime API directly.

## Overview

Aether compiles to C, which means integration with C codebases is straightforward:

1. **Compile-and-Link**: Write actors in Aether, compile to C with `aetherc`, include the generated code
2. **Direct Runtime API**: Use Aether's runtime API to create actors entirely in C

## Approach 1: Compile Aether to C

### Step 1: Write Your Aether Actor

**counter.ae:**
```aether
message Increment {
    amount: int
}

message GetValue {}

message Reset {}

actor Counter {
    state count = 0

    receive {
        Increment(amount) -> {
            count = count + amount
        }
        GetValue() -> {
            print(count)
        }
        Reset() -> {
            count = 0
        }
    }
}

main() {
    counter = spawn(Counter())
    counter ! Increment { amount: 10 }
    counter ! GetValue {}
}
```

### Step 2: Compile to C

```bash
aetherc counter.ae counter.c
```

This generates `counter.c` containing:
- Message struct definitions
- Actor struct with state
- Message handler dispatch table
- `spawn_Counter()` function

### Step 3: Include in Your C Project

**main.c:**
```c
#include <stdio.h>
#include "runtime/aether_runtime.h"
#include "runtime/scheduler/multicore_scheduler.h"

// Forward declarations from generated code
typedef struct Counter Counter;
Counter* spawn_Counter(void);

// Message IDs (from generated code, numbered from 0 in declaration order)
#define MSG_Increment 0
#define MSG_GetValue 1
#define MSG_Reset 2

int main(int argc, char** argv) {
    // Initialize Aether runtime
    aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);
    scheduler_init(4);  // 4 cores
    scheduler_start();

    // Spawn the actor
    Counter* counter = spawn_Counter();

    // Send messages using the Message struct
    Message msg = {0};
    msg.type = MSG_Increment;
    msg.payload_int = 10;  // amount
    scheduler_send_remote((ActorBase*)counter, msg, -1);

    // Allow time for processing
    // In real code, use proper synchronization
    usleep(100000);

    // Cleanup
    scheduler_shutdown();  // Waits for quiescence, stops and joins threads
    scheduler_cleanup();
    aether_runtime_shutdown();

    return 0;
}
```

### Step 4: Build

```bash
# Compile Aether code
aetherc counter.ae counter.c

# Build with runtime, `ae cflags` emits the right -I / -L / -l for
# the install in effect. Don't hand-craft the linker flags: the
# install layout (`<prefix>/lib/aether/libaether.a`) requires an
# explicit `-L<prefix>/lib/aether` that bare `-laether` won't find.
gcc -O2 main.c counter.c $(ae cflags) -o myapp
```

## Approach 2: Direct Runtime API

For full control, create actors directly in C using the runtime API.

### Minimal Example

```c
#include "runtime/aether_runtime.h"
#include "runtime/scheduler/multicore_scheduler.h"

// Define your actor structure
typedef struct {
    ActorBase base;    // Must be first field
    int counter;       // Your state
} MyCounter;

// Message handler: one message per call (see "Message Handler Pattern")
void my_counter_step(void* self) {
    MyCounter* actor = (MyCounter*)self;
    Message msg;

    if (!mailbox_receive(&actor->base.mailbox, &msg)) return;
    switch (msg.type) {
        case 1:  // Increment
            actor->counter += msg.payload_int;
            break;
        case 2:  // Print
            printf("Counter: %d\n", actor->counter);
            break;
    }
}

int main() {
    // Initialize runtime
    aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);
    scheduler_init(4);
    scheduler_start();

    // Spawn actor using pool
    MyCounter* actor = (MyCounter*)scheduler_spawn_actor(
        0,                    // Preferred core
        my_counter_step,      // Step function
        sizeof(MyCounter)     // Size
    );
    actor->counter = 0;       // Initialize state

    // Send messages
    Message msg = {0};
    msg.type = 1;             // Increment
    msg.payload_int = 42;
    scheduler_send_remote((ActorBase*)actor, msg, -1);

    msg.type = 2;             // Print
    scheduler_send_remote((ActorBase*)actor, msg, -1);

    // Wait for messages to be processed
    scheduler_wait();  // Non-destructive: can send more messages after this

    // Cleanup
    scheduler_shutdown();  // Waits for quiescence, stops and joins threads
    scheduler_cleanup();
    aether_runtime_shutdown();

    return 0;
}
```

## Runtime API Reference

### Lifecycle Functions

```c
// Initialize runtime with automatic CPU feature detection
// num_cores: 0 = auto-detect, or specify explicitly
// flags: AETHER_FLAG_AUTO_DETECT recommended
void aether_runtime_init(int num_cores, int flags);

// Shutdown runtime and free resources
void aether_runtime_shutdown(void);

// Initialize scheduler with core count
// cores: 0 = auto-detect
void scheduler_init(int cores);

// Start scheduler threads
void scheduler_start(void);

// Wait for quiescence (all pending messages processed)
// Non-destructive: scheduler threads keep running. Safe to call multiple times.
void scheduler_wait(void);

// Wait for quiescence, then stop and join scheduler threads.
// Call once at program exit.
void scheduler_shutdown(void);

// Free scheduler resources
void scheduler_cleanup(void);
```

### Actor Management

```c
// Spawn actor from pool (recommended)
// Returns actor pointer, NULL on failure
ActorBase* scheduler_spawn_actor(
    int preferred_core,      // Core hint, -1 for caller's core (main thread defaults to core 0)
    void (*step)(void*),     // Message handler function
    size_t actor_size        // sizeof(YourActorType)
);

// End an actor: out of its core's table now, freed once no scheduler
// thread can still be looking at it. Safe from the actor's own step. Call it
// only when nothing sends to the actor any more.
void scheduler_release_actor(ActorBase* actor);
AetherActorRef scheduler_actor_ref(ActorBase* actor);   // the actor and its id
void scheduler_send_ref(AetherActorRef ref, Message msg); // dropped once the actor is released

// Register existing actor (alternative to pooled)
int scheduler_register_actor(ActorBase* actor, int preferred_core);
```

### Message Passing

```c
// Message structure
typedef struct {
    int type;           // Message ID
    int sender_id;      // Sender actor ID
    intptr_t payload_int; // For single-int messages (intptr_t to pass actor refs without truncation)
    void* payload_ptr;  // For complex payloads
    struct { void* data; int size; int owned; } zerocopy;  // owned: the payload's kind
    void* _reply_slot;  // An ask's reply slot
} Message;

// Send to actor on same core (fast path)
void scheduler_send_local(ActorBase* actor, Message msg);

// Send to actor on any core
// from_core: sender's core ID, -1 if unknown
void scheduler_send_remote(ActorBase* actor, Message msg, int from_core);
```

### Configuration Flags

```c
#define AETHER_FLAG_AUTO_DETECT      (1 << 0)  // Detect CPU features
#define AETHER_FLAG_LOCKFREE_MAILBOX (1 << 1)  // Lock-free mailboxes
#define AETHER_FLAG_LOCKFREE_POOLS   (1 << 2)  // Deprecated: accepted and ignored (#3374)
#define AETHER_FLAG_ENABLE_SIMD      (1 << 3)  // SIMD optimizations
#define AETHER_FLAG_ENABLE_MWAIT     (1 << 4)  // MWAIT for idle
#define AETHER_FLAG_VERBOSE          (1 << 5)  // Print config on init
```

## Actor Structure Requirements

When defining actors in C, follow these rules:

```c
typedef struct {
    ActorBase base;     // MUST be first field
    // Your state fields here
    int my_value;
    char* my_string;
} MyActor;
```

The `ActorBase` contains:
- `active`: Processing flag (`atomic_int`)
- `id`: Unique actor ID
- `mailbox`: Message queue (ring buffer)
- `step`: Your message handler
- `assigned_core`: Current core assignment (`atomic_int`, safe for cross-core reads)
- `migrate_to`: Affinity hint for message-driven migration (`atomic_int`)
- `spsc_queue`: unused, NULL (kept for the layout, #3292)
- `reply_slot`: Non-NULL during ask/reply (`_Atomic` pointer)
- `step_lock`: Prevents concurrent `step()` during work-steal handoff (`atomic_flag`)
- `timeout_ns`, `last_activity_ns`: Receive timeout and when idling began.
  Armed outside the actor's own step (at spawn, by hand), the countdown is
  told to the actor's core with `scheduler_timeout_armed(actor)`: a core
  looks at the actors it is told of, not at every actor it has (#3540)
- `timer_slot`, `ready_queued`: The core's timer heap and ready list (#3540);
  the scheduler's own, zero in a new actor
- `dead`: Set once the actor panicked or was released; it is skipped after
- `alloc_size`, `destroy_state`, `scheduler_owned`: How the scheduler frees it
- `inbox`, `thread_parked`: For an actor with its own thread (`auto_process`),
  where every sender puts its messages and whether the thread sleeps idle.
  That thread is not its core's scheduler thread, so what its step sends
  goes as a foreign thread's sends do, through the locked channel (#3246),
  and an ask it makes waits without stepping anything for the core
- `step_payload`, `step_type`: The message a generated handler is running on,
  for the step barrier to drop if the handler panics (#3142); `step_inline`
  says the payload is the actor's own copy of one that travelled in its
  message (#3534), which the barrier does not free
- `release_message`: Releases what a dropped message's fields own; NULL for
  an actor you register, whose messages are yours
- `inflight`: Messages for the actor on their way through a core's queues;
  it moves to another core only while this is 0

Embedding `ActorBase` as the first member, as above, keeps the layout right
whatever fields it gains. The full list, in order, with a comment for each, is
the `AETHER_ACTOR_BASE_FIELDS` macro in `runtime/scheduler/multicore_scheduler.h`.

## Message Handler Pattern

A step takes **one** message per call, as a generated step does: the
scheduler calls it once for each message it delivers and counts each call as
one message processed. A step that drained its whole mailbox would leave
`scheduler_wait()`, and so `scheduler_shutdown()`, waiting for messages it
counts as never processed.

```c
void my_actor_step(void* self) {
    MyActor* actor = (MyActor*)self;
    Message msg;

    if (!mailbox_receive(&actor->base.mailbox, &msg)) return;
    switch (msg.type) {
        case MSG_TYPE_1:
            // Handle message type 1
            handle_type_1(actor, msg.payload_int);
            break;

        case MSG_TYPE_2:
            // Handle message type 2: a typed message's struct, sent with
            // aether_send_message
            handle_type_2(actor, (SomeStruct*)aether_message_payload(&msg));
            aether_message_free_payload(&msg);
            break;

        default:
            // Unknown message type
            break;
    }
}
```

A typed message whose struct fits the bytes from `payload_int` up to
`zerocopy.owned` (28 on 64-bit targets, 16 on 32-bit) travels in the
`Message` itself, with `zerocopy.owned` set to `AETHER_MSG_INLINE`; a larger
one is a heap copy in `payload_ptr` (#3534). `aether_message_payload(&msg)`
reads either, and `aether_message_free_payload(&msg)` gives either back once
its fields are read or released; never read or free `payload_ptr` of a typed
message directly. `zerocopy.owned` compares to `AETHER_MSG_ZC_BORROWED`,
`AETHER_MSG_ZC_OWNED` (`zerocopy.data` is the message's to free) and
`AETHER_MSG_INLINE` (`runtime/actors/actor_state_machine.h`).

## Build Configuration

### Makefile Example

`ae cflags` is the canonical way to obtain the include and link flags
it covers in-tree dev builds, `~/.aether` user installs, and
`/usr/local` system installs uniformly. Do not hand-craft `-I` / `-L`
/ `-l`: the install layout puts the archive at `<prefix>/lib/aether/`
(not flat under `<prefix>/lib/`), so `-laether` alone wouldn't find
it without an explicit `-L`.

```makefile
CC = gcc
AE_CFLAGS := $(shell ae cflags)

# For Aether source files
%.c: %.ae
	aetherc $< $@

myapp: main.c counter.c
	$(CC) -O2 -o $@ $^ $(AE_CFLAGS)
```

### CMake Example

```cmake
cmake_minimum_required(VERSION 3.10)
project(MyAetherApp)

set(AETHER_HOME "$ENV{HOME}/.aether")

include_directories(${AETHER_HOME}/include/aether)
link_directories(${AETHER_HOME}/lib)

add_executable(myapp main.c counter.c)
target_link_libraries(myapp aether pthread)
```

## Thread Safety

- Each actor runs on one core at a time
- Messages are delivered via thread-safe queues
- Actor state is only accessed by one thread
- Cross-core sends use lock-free queues

## Host-process boundary

The C host and the Aether runtime share a process but have distinct ownership rules. Respect these at every integration point:

1. **State is message-only.** Actor state is owned by the actor; the host reads and writes it exclusively through messages. Reach-in access bypasses message-ordering guarantees and races with the scheduler.
2. **Sends are fire-and-forget.** There's no synchronous reply from `aether_send_message()`. When you need a value back, either (a) have the actor send a response message to a host-registered event handler, or (b) have the actor write into shared atomic state the host polls.
3. **References are manually lifetime-managed.** The host holds the raw `ActorBase*` (or the generated `Counter*`) returned by spawn; there's no automatic refcount across the FFI boundary. Keep the pointer as long as you intend to send to the actor. Release it with `scheduler_release_actor()` when done, after the last message you send it has been handled: the actor is reclaimed later, once the scheduler threads are past it. Nothing may send to it after the release. A send that does so anyway is defined, not a use after free: a released actor's memory is kept, marked released, until a later spawn of the same size reuses it, so the message is dropped (counted by `scheduler_released_sends()`, reported once on stderr), but a send made after the memory has become another actor reaches that actor, which a raw pointer cannot detect. A host that may send late holds an `AetherActorRef` instead (`scheduler_actor_ref(actor)`, the actor and its id) and sends with `scheduler_send_ref(ref, msg)`: a new actor in the same memory has an id of its own, so such a send is dropped and counted as well (#2527).
4. **One runtime per process.** `aether_runtime_init()` initializes process-global scheduler state. Calling it twice from the same process is not supported; use separate processes if you need isolated runtimes.

## Header Generation

Pass `--emit-header` to `aetherc` to generate a C header alongside the C output. The header contains message struct definitions, `MSG_*` constants, actor spawn prototypes and a typed send helper per message an actor receives, everything needed to send messages to Aether actors from C without copying struct definitions by hand. The message structs are the ones the generated C declares, field for field: the fields are packed (ints and bools first, then pointer-sized fields, then the rest), not in declaration order, so build a message by field name, or through its helper, whose parameters are in declaration order.

```bash
aetherc --emit-header counter.ae counter.c
# Produces: counter.c  counter.h
```

`--emit-header` must precede the input/output paths. Placed after the positional
arguments it is ignored and only `counter.c` is written.

Example generated header:

```c
// counter.h (auto-generated by aetherc --emit-header)
#ifndef COUNTER_H
#define COUNTER_H

#include <stdint.h>
#include "runtime/scheduler/multicore_scheduler.h"
#include "runtime/actors/aether_send_message.h"

// Message IDs (numbered from 0 in declaration order)
#define MSG_Increment 0
#define MSG_Label     1

// Message structs, first field is the message ID; single-int payloads
// widen to intptr_t to match Message.payload_int.
typedef struct { int _message_id; intptr_t amount; } Increment;
// `message Label { tag: int, text: string, weight: float }`: packed, so
// the string lands before the float.
typedef struct { int _message_id; int tag; const char* text; double weight; } Label;

// Spawn prototype and typed send helpers (parameters in declaration order)
Counter* spawn_Counter(void);
static inline void Counter_Increment(Counter* actor, intptr_t amount) { ... }
static inline void Counter_Label(Counter* actor, int tag, const char* text, double weight) { ... }

#endif
```

Include the header in your C host application and send through the helpers, or build a struct by field name and pass it to `aether_send_message(actor, &msg, sizeof msg)`. A `string` field takes a `const char*` the host keeps alive until the message has been handled; the actor does not free it.

---

## Polling from a C Event Loop

With threads, actors run on the scheduler's threads from the first spawn on, so a C event loop (raylib, SDL, a game loop) that holds the main thread does not hold them up, and has nothing to poll: `aether_scheduler_poll` returns 0 there (#3112). In the cooperative (no-threads) runtime the actors run only when the host lets them, so a C loop calls `aether_scheduler_poll()` between frames:

```c
#include "runtime/scheduler/multicore_scheduler.h"

// In your render/game loop:
while (!window_should_close()) {
    // Drain actor messages, process up to 64 per actor per call.
    // Pass 0 for unlimited (processes all pending).
    aether_scheduler_poll(64);

    begin_drawing();
    // ... render ...
    end_drawing();
}
```

```c
// Signature
//   max_per_actor: max messages to process per actor per call (0 = unlimited)
//   returns: total messages processed across all actors
int aether_scheduler_poll(int max_per_actor);
```

In the cooperative runtime `aether_scheduler_poll` steps every actor that has messages, at most `max_per_actor` of them each; with threads it returns 0. It is a no-op if no actors are registered.

## See Also

- [C Interoperability](c-interop.md) - Calling C from Aether
- [Runtime Configuration](runtime-config.md) - Runtime flags and options
- [Actor Concurrency](actor-concurrency.md) - Actor model concepts
