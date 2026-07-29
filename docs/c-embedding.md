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

// Message handler
void my_counter_step(void* self) {
    MyCounter* actor = (MyCounter*)self;
    Message msg;

    // Process messages from mailbox
    while (mailbox_receive(&actor->base.mailbox, &msg)) {
        switch (msg.type) {
            case 1:  // Increment
                actor->counter += msg.payload_int;
                break;
            case 2:  // Print
                printf("Counter: %d\n", actor->counter);
                break;
        }
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

// Return actor to pool when done
void scheduler_release_actor(ActorBase* actor);

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
#define AETHER_FLAG_LOCKFREE_POOLS   (1 << 2)  // Lock-free pools
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
- `main_thread_only`: If set, scheduler threads skip this actor (`atomic_int`)
- `spsc_queue`: Lock-free same-core messaging (pointer, lazily allocated)
- `reply_slot`: Non-NULL during ask/reply (`_Atomic` pointer)
- `step_lock`: Prevents concurrent `step()` during work-steal handoff (`atomic_flag`)

## Message Handler Pattern

```c
void my_actor_step(void* self) {
    MyActor* actor = (MyActor*)self;
    Message msg;

    // Drain all pending messages
    while (mailbox_receive(&actor->base.mailbox, &msg)) {
        switch (msg.type) {
            case MSG_TYPE_1:
                // Handle message type 1
                handle_type_1(actor, msg.payload_int);
                break;

            case MSG_TYPE_2:
                // Handle message type 2
                handle_type_2(actor, (SomeStruct*)msg.payload_ptr);
                break;

            default:
                // Unknown message type
                break;
        }
    }
}
```

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
3. **References are manually lifetime-managed.** The host holds the raw `ActorBase*` (or the generated `Counter*`) returned by spawn; there's no handle type or automatic refcount across the FFI boundary. Keep the pointer as long as you intend to send to the actor. Release it with `scheduler_release_actor()` when done.
4. **One runtime per process.** `aether_runtime_init()` initializes process-global scheduler state. Calling it twice from the same process is not supported; use separate processes if you need isolated runtimes.

## Header Generation

Pass `--emit-header` to `aetherc` to generate a C header alongside the C output. The header contains message struct definitions, `MSG_*` constants, and actor spawn prototypes, everything needed to send messages to Aether actors from C without copying struct definitions by hand.

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

// Message IDs (numbered from 0 in declaration order)
#define MSG_Increment 0
#define MSG_GetValue  1
#define MSG_Reset     2

// Message structs, first field is the message ID; single-int payloads
// widen to intptr_t to match Message.payload_int.
typedef struct { int _message_id; intptr_t amount; } Increment;

// Spawn prototype
Counter* spawn_Counter(void);

#endif
```

Include the header in your C host application and use the constants with `scheduler_send_remote`.

---

## Polling from a C Event Loop

When a C event loop (raylib, SDL, game loop, etc.) holds the main thread, Aether actors in main-thread mode cannot process messages, the scheduler has no opportunity to run.

Use `aether_scheduler_poll()` to drain pending messages from C code between frames:

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

`aether_scheduler_poll` is safe to call from the main thread at any time. It only processes actors that are in main-thread mode; scheduler worker threads are not affected. It is a no-op if no actors are registered.

## See Also

- [C Interoperability](c-interop.md) - Calling C from Aether
- [Runtime Configuration](runtime-config.md) - Runtime flags and options
- [Actor Concurrency](actor-concurrency.md) - Actor model concepts
