# Aether Architecture

This document provides an overview of Aether's compiler pipeline, runtime design, and key architectural decisions.

## System Overview

```
+--------------------------------------------------------+
|                    Aether Compiler                     |
|                                                        |
|  .ae -> Lexer -> Parser -> ModuleOrch -> TypeCheck -+  |
|                                                    |   |
|         .c  <- CodeGen <- Optimizer <--------------+   |
+--------------------------------------------------------+
                          |
+--------------------------------------------------------+
|                       GCC/Clang                        |
|              .c source -> native binary                |
+--------------------------------------------------------+
                          |
+--------------------------------------------------------+
|                     Aether Runtime                     |
|  Scheduler -> Actors -> Memory Pools -> Message Passing|
+--------------------------------------------------------+
```

## Compiler Pipeline

### 1. Lexer (`compiler/parser/lexer.c`)

**Purpose:** Convert source text into tokens

**Input:** Raw Aether source code (`.ae` file)

**Output:** Stream of tokens

**Process:**
1. Read source character by character
2. Identify token boundaries (whitespace, operators, keywords)
3. Classify tokens (identifier, number, string, keyword, operator)
4. Track line and column numbers for error reporting

**Example:**
```aether
x = 42 + y
```

Produces tokens:
```
IDENTIFIER("x") EQUALS NUMBER(42) PLUS IDENTIFIER("y")
```

**Key Files:**
- `compiler/parser/lexer.c` - Implementation
- `compiler/parser/tokens.h` - Token type definitions

### 2. Parser (`compiler/parser/parser.c`)

**Purpose:** Build Abstract Syntax Tree (AST) from tokens

**Input:** Token stream from lexer

**Output:** AST representing program structure

**Process:**
1. Consume tokens in order
2. Apply grammar rules (recursive descent parsing)
3. Build tree structure with each node representing a language construct
4. Report syntax errors with location information

**Key Concepts:**
- **Recursive Descent:** Each grammar rule is a function
- **Precedence Climbing:** Handles operator precedence
- **Error Recovery:** Attempts to continue parsing after errors

**Key Files:**
- `compiler/parser/parser.c` - Implementation
- `compiler/ast.h` - AST node definitions

### 2.5 Module Orchestrator (`compiler/aether_module.c`)

**Purpose:** Resolve all imports, parse module files, cache ASTs, detect circular dependencies

**Input:** AST from parser (scanned for `AST_IMPORT_STATEMENT` nodes)

**Output:** Populated `global_module_registry` with cached module ASTs

**Process:**
1. Scan program AST for import statements
2. Resolve each import to a file path (stdlib paths, lib/, src/)
3. Parse each module file (lex → parse → AST) with lexer state save/restore
4. Cache parsed ASTs in `global_module_registry`
5. Recursively process each module's own imports
6. Build dependency graph and detect circular imports via DFS

**Key Files:**
- `compiler/aether_module.c` - Orchestration, resolution, parsing, dependency graph
- `compiler/aether_module.h` - Module registry, dependency graph types

### 3. Type Checker (`compiler/analysis/typechecker.c`)

**Purpose:** Infer and verify types for local variables

**Input:** AST from parser

**Output:** Type-annotated AST

**Process:**
1. Two-pass: first collects declarations, then checks all nodes
2. Constraint-based type inference (Hindley-Milner style, max 100 iterations)
3. Report type errors with location information

**Key Algorithms:**
- **Type Inference:** Automatic type deduction for local variables
- **Two-Pass Checking:** Declarations collected before use-sites are checked

**Key Files:**
- `compiler/analysis/typechecker.c` - Type checking (two-pass)
- `compiler/analysis/type_inference.c` - Constraint-based inference

### 4. Optimizer (`compiler/codegen/optimizer.c`)

**Purpose:** Apply AST-level optimizations before code generation

**Input:** Type-checked AST

**Output:** Optimized AST

**Optimizations (always on):**

1. **Constant Folding**, `2 + 3 * 4` → `14` at compile time
2. **Dead Code Elimination**, `if false { ... }` block removed
3. **Tail Call Detection**, identifies recursive tail calls (detection only; loop transformation is pending)

**Optimizations (applied during codegen, `compiler/codegen/codegen_stmt.c`):**

4. **Arithmetic Series Collapse**, `while i < n { acc += C; i++ }` → O(1) closed form
5. **Linear Counter Sum**, `while i < n { total += i; i++ }` → triangular-number formula

**Key Files:**
- `compiler/codegen/optimizer.c` - AST-level passes
- `compiler/codegen/codegen_stmt.c` - Series and linear loop collapse

### 5. Code Generator (`compiler/codegen/codegen.c`)

**Purpose:** Generate C code from AST

**Input:** Optimized AST

**Output:** C source code (`.c` file)

**Process:**
1. Walk AST in depth-first order
2. Generate equivalent C code for each node
3. Emit actor runtime calls for actor constructs
4. Generate computed goto dispatch tables for message handlers
5. Detect single-int messages and emit inline fast paths

**Key Features:**
- Computed goto dispatch for message handlers
- Inline single-int message encoding (bypasses pool allocation)
- `scheduler_send_local` / `scheduler_send_remote` routing based on `current_core_id`
- Locality-aware actor placement via `scheduler_spawn_actor` (caller's core, or core 0 for main thread)
- NUMA-aware actor allocation with derived-struct size

**Key Files:**
- `compiler/codegen/codegen.c` - Entry point and includes
- `compiler/codegen/codegen_expr.c` - Expression generation
- `compiler/codegen/codegen_stmt.c` - Statement generation and loop optimizations
- `compiler/codegen/codegen_actor.c` - Actor dispatch tables
- `compiler/codegen/codegen_func.c` - Function definitions
- `compiler/codegen/codegen.h` - API

**Deterministic output:**

The same compiler build given the same source emits byte-identical C,
every time. Two invariants make this true, and both are load-bearing:

1. **Emission order tracks AST/source order.** No codegen pass (or
   analysis pass that feeds codegen) iterates a hash-keyed container to
   emit declarations, helpers, string literals, or dedup tables. If a
   future pass needs keyed lookup, it must still emit in source order.
2. **No volatile metadata reaches emitted text.** The compiler bakes no
   timestamps, hostnames, or absolute build paths into generated C. The
   version string flows through the build-generated
   `runtime/aether_version.h` (numeric components only), not through
   codegen.

Scope boundary: the guarantee is per compiler build. A different
`aetherc` build (new version, different optimization passes) may emit
different C; the downstream C compiler's binary output is out of scope.

The property is enforced in CI by
`tests/integration/emit_c_determinism/test_emit_c_determinism.sh`,
which compiles a corpus twice and byte-compares the results, and
rejects timestamp macros outright.

## Runtime Architecture

### Actor System

**Design:** Lightweight process model with per-core scheduling

**Key Components:**

1. **Actor Structure** (`runtime/scheduler/multicore_scheduler.h`)
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
   } ActorBase;
   ```

2. **Message Structure** (`runtime/actors/actor_state_machine.h`)
   ```c
   typedef struct {
       int type;
       int sender_id;
       intptr_t payload_int;
       void* payload_ptr;
       struct {
           void* data;
           int size;
           int owned;
       } zerocopy;
   } Message;
   ```

3. **Message Queue** (`runtime/scheduler/lockfree_queue.h`)
   - Lock-free SPSC ring buffer with cache-line aligned head/tail
   - Batch enqueue and dequeue to reduce atomic operations
   - Power-of-2 sizing with bitwise AND masking

4. **Message Dispatch** (generated code)
   - Computed goto dispatch table for direct label jumps
   - Message ID read from `msg.type` (not pointer dereference)

### Scheduler

**Design:** Partitioned multicore scheduler with work stealing and message-driven migration

**Architecture:**
```
+---------------------------------------------------------+
|              Partitioned Scheduler                      |
|  +------------+------------+------------+------------+  |
|  |  Core 0    |  Core 1    |  Core 2    |  Core 3    |  |
|  |  [A1, A2]  |  [A3, A4]  |  [A5, A6]  |  [A7]      |  |
|  |  (local)   |  (local)   |  (local)   |  (idle)    |  |
|  |            |            |  <- steal --------+     |  |
|  +------------+------------+------------+------------+  |
+---------------------------------------------------------+
```

**Locality-Aware Placement:**
1. Actors placed on the caller's core at spawn time
2. Main thread spawns default to core 0, keeping top-level actor groups co-located
3. Actors spawned from within actor handlers inherit the parent's core
4. Each core processes only its local actors in the fast path
5. Same-core messages delivered directly to mailbox (no queue overhead)
6. Cross-core messages enqueued to target core's lock-free incoming queue

**Message-Driven Migration:**
1. Cross-core sender sets `migrate_to` hint on target actor, requesting migration to the sender's core
2. Owning scheduler checks migration hints after coalesce-buffer processing (targeted, O(batch_size)) and during idle actor scans (full, O(actor_count))
3. Actor relocated using ascending core-id lock ordering with non-blocking try-lock
4. Migrated-actor messages forwarded with spin-retry to prevent drops

**Work Stealing (idle cores):**
1. Core becomes idle after extended empty cycles
2. Scans all cores to find busiest (most pending work)
3. Non-blocking try-lock attempt using ascending core-id ordering
4. Steals one actor if victim has more than 4 actors
5. Stolen actor reassigned to idle core

**NUMA-Aware Allocation:**
1. Detects NUMA topology at scheduler initialization
2. Allocates actor structures on the NUMA node local to the assigned core
3. Falls back to standard malloc on single-node systems
4. Platform support: Linux (libnuma), Windows (VirtualAllocExNuma), macOS (fallback)

**Key Files:**
- `runtime/scheduler/multicore_scheduler.c` - Multi-core implementation
- `runtime/scheduler/aether_scheduler_coop.c` - Cooperative single-threaded backend
- `runtime/scheduler/multicore_scheduler.h` - API and data structures (shared by both)
- `runtime/scheduler/lockfree_queue.h` - Cross-core message queue
- `runtime/aether_numa.c` - NUMA detection and allocation

### Cooperative Scheduler (Threadless Platforms)

For platforms without pthreads (WebAssembly, embedded, bare-metal), the cooperative scheduler (`aether_scheduler_coop.c`) implements the same API as the multi-core scheduler but runs everything on a single thread.

**Architecture:**
```
+-----------------------------------+
|    Cooperative Scheduler          |
|  +---------+---------+---------+  |
|  | Actor 1 | Actor 2 | Actor 3 |  |
|  | (poll)  | (poll)  | (poll)  |  |
|  +---------+---------+---------+  |
|  All on core 0, round-robin poll  |
+-----------------------------------+
```

**Key Differences from Multi-Core:**
- `scheduler_init()` forces `num_cores = 1` and `main_thread_mode = true`
- `scheduler_start()` / `scheduler_stop()` are no-ops (no threads to manage)
- `scheduler_wait()` drains via `aether_scheduler_poll()` loop
- All sends are local (no cross-core queues, no migration)
- Ask/reply is synchronous (poll until reply_ready)
- `aether_send_message_sync()` heap-allocates message data (no zero-copy stack optimization, mailbox FIFO order means the sent message may not be the next one consumed)

**Selection:** The Makefile selects the scheduler based on `PLATFORM` or auto-detects `AETHER_NO_THREADING` in `EXTRA_CFLAGS`. Only one scheduler .c file is compiled into the binary. Users can also use `ae build --target wasm` for Emscripten builds.

### Actor Timeouts

Actors with `receive { ... } after N -> { ... }` carry `timeout_ns` and `last_activity_ns` fields in `ActorBase`. The generated step function checks `_aether_clock_ns()` before `mailbox_receive()`. If the idle duration exceeds the threshold, the timeout handler fires (one-shot, cancelled when any message arrives). Both schedulers poll timeout-active actors even when their mailbox is empty.

### Cooperative Preemption (Opt-In)

Two independent mechanisms prevent long-running handlers from starving other actors:

1. **Scheduler-side** (`AETHER_PREEMPT=1`): Time-based drain loop break after configurable threshold (default 1ms)
2. **Codegen-side** (`aetherc --preempt`): `sched_yield()` inserted at loop back-edges with reduction counter (every 10000 iterations)

Both are zero-cost when disabled. This matches Go's cooperative preemption model (pre-1.14).

### Platform Detection (Tier 0)

`runtime/config/aether_optimization_config.h` defines compile-time `AETHER_HAS_*` flags that gate all platform-dependent code. These are resolved before any other header is included.

| Flag | Default 1 when | Default 0 when |
|------|----------------|----------------|
| `AETHER_HAS_THREADS` | Linux/macOS/Windows | `__EMSCRIPTEN__`, `-DAETHER_NO_THREADING` |
| `AETHER_HAS_ATOMICS` | C11 stdatomic available | `__STDC_NO_ATOMICS__`, `-DAETHER_NO_ATOMICS` |
| `AETHER_HAS_FILESYSTEM` | Hosted platforms | `-DAETHER_NO_FILESYSTEM` |
| `AETHER_HAS_NETWORKING` | Linux/macOS/Windows | `__EMSCRIPTEN__`, `-DAETHER_NO_NETWORKING` |
| `AETHER_HAS_NUMA` | Linux/Windows | macOS, `__EMSCRIPTEN__`, `-DAETHER_NO_NUMA` |
| `AETHER_HAS_SIMD` | x86_64/aarch64 | `-DAETHER_NO_SIMD`, `__EMSCRIPTEN__` |
| `AETHER_HAS_AFFINITY` | Linux/macOS/Windows | `-DAETHER_NO_AFFINITY`, `__EMSCRIPTEN__` |
| `AETHER_HAS_GETENV` | Hosted platforms | `-DAETHER_NO_GETENV`, freestanding |
| `AETHER_HAS_MALLOC` | Everywhere | `-DAETHER_NO_MALLOC` |

When `AETHER_HAS_ATOMICS == 0`, `<stdatomic.h>` is replaced with fallback typedefs (`atomic_int` → `volatile int`). When `AETHER_HAS_THREADS == 0`, `aether_thread.h` provides no-op pthread stubs via macro redirects.

### Memory Management

**Message Payloads:**
- Thread-local pool of 256 pre-allocated buffers (256 bytes each)
- Pool acquisition and release use plain loads/stores (no atomics for TLS)
- Falls back to malloc for oversized messages or pool exhaustion
- `aether_free_message` returns buffers to the pool or calls `free`

**Actor Allocation:**
- `scheduler_spawn_actor` allocates via `aether_numa_alloc` with the full derived-struct size
- NUMA-aware placement on the local node of the assigned core
- Falls back to standard allocation on non-NUMA systems

**String Ownership (heap-string tracker, issue #405):**
- Every string variable carries a compiler-emitted `_heap_<name>` companion that flips between 0 (literal) and 1 (heap-allocated) on each reassignment
- Trackers are emitted at function-entry scope by a dedicated codegen pre-pass (`hoist_heap_string_trackers` in `compiler/codegen/codegen_stmt.c`), making cross-block reassignment safe
- Reassignment goes through a unified wrapper handling all four heap/literal transitions: `{ const char* _tmp_old = s; s = <rhs>; if (_heap_s) free(_tmp_old); _heap_s = <rhs_is_heap>; }`
- Recognised heap sources: hardcoded stdlib `string.{concat,substring,to_upper,to_lower,trim}`, string interpolation `"foo ${x}"`, and user-defined `-> string` functions whose body provably returns heap (recursive structural escape analysis with cycle detection, memoised on the function-definition AST node's annotation slot)
- Runtime cost: zero, just `if (...) free(...)` per assignment. Codegen-time cost: O(K) per call site for fn-def lookup where K is the number of top-level fn definitions; total well under a millisecond for typical programs

### Performance Optimizations

The runtime implements these optimization strategies in the active code paths:

1. **Main Thread Actor Mode** - Single-actor programs bypass scheduler entirely
2. **Thread-Local Message Pools** - Eliminate malloc/free overhead
3. **Batch Dequeue** - Reduce atomic operations from N to 1 per batch
4. **Adaptive Batching** - Scale batch size with load (64 to 1024)
5. **Direct Mailbox Delivery** - Same-core messages bypass the queue
6. **Computed Goto Dispatch** - Direct label jumps for message handlers
7. **Inline Single-Int Messages** - Bypass pool allocation for common messages
8. **Message-Driven Migration** - Co-locate communicating actors
9. **Optimized Spinlock** - PAUSE/YIELD hints during spin-wait
10. **Cache Line Alignment** - Prevent false sharing on shared structures
11. **Relaxed Atomic Ordering** - Avoid barriers on non-critical counters

See [runtime-optimizations.md](runtime-optimizations.md) for implementation details.

## Module System

**Design:** Static module resolution with circular import detection

**Resolution Process:**
1. Parse import statement, e.g. `import std.collections`.
2. Search paths: `std/collections/module.ae` first, then `./collections/module.ae` relative to the importer, then `contrib/<name>/module.ae` for contrib modules.
3. Load and parse the module if it isn't already loaded.
4. Build the dependency graph and detect circular imports via DFS with visited tracking.
5. Merge the module's exported functions and constants into the program AST with namespace-prefixed names (`module.func` → `module_func` internally).
6. Emit C declarations/definitions for the merged symbols at codegen.

**Key Files:**
- `compiler/aether_module.c` - Module resolution
- `compiler/aether_module.h` - Module API

## Design Decisions

### Why Compile to C?

**Advantages:**
- Leverage mature C compilers (GCC, Clang)
- Access to optimizations (inlining, loop unrolling, auto-vectorization)
- Portable across platforms
- Direct FFI with C libraries

**Disadvantages:**
- Two-stage compilation adds latency
- Debugging requires looking at generated C
- Some optimizations harder to implement at the source level

### Why Work Stealing?

**Advantages:**
- Automatic load balancing for uneven workloads
- Cache-friendly for local work
- Non-blocking: failed steals do not block progress

**Alternatives Considered:**
- Round-robin: poor load balancing
- Per-core task queue: no load balancing
- Global queue: high contention

### Why Lock-Free Queues?

**Advantages:**
- No mutex contention in the message-passing hot path
- Scales with core count
- Batch operations amortize atomic overhead

**Disadvantages:**
- Complexity (memory ordering, ABA considerations)
- Requires careful attention to cache line alignment

## Trade-Offs

### Compilation Speed vs Runtime Performance
- **Choice:** Favor runtime performance
- **Implication:** Two-stage compilation with aggressive optimization flags
- **Mitigation:** Incremental builds, parallel compilation

### Memory Safety vs Performance
- **Choice:** Arena-based memory management (bulk deallocation, no tracing GC)
- **Implication:** Deterministic cleanup, no GC pauses
- **Mitigation:** Valgrind, AddressSanitizer for leak detection during development

### Portability vs Optimization
- **Choice:** Portable C11 code with platform-specific branches
- **Implication:** Platform-specific intrinsics guarded by `#if` directives
- **Mitigation:** Fallback paths for all platform-specific code

## References

- Type Inference: Pierce & Turner (2000)
- Michael-Scott Queue: Michael & Scott (1996)
- Work Stealing: Blumofe & Leiserson (1999)
- Actor Model: Hewitt, Bishop, Steiger (1973)
- Arena Allocation: Hanson (1990)
