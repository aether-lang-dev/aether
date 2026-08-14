# Runtime Configuration - Quick Reference

## What Does "Lock-Free" Mean?

**With Locks (Old):**
```
Thread 1: Lock → Do Work → Unlock
Thread 2: [WAITING...] Lock → Do Work → Unlock  ← Blocked!
```
One thread blocks others.

**Lock-Free (New):**
```
Thread 1: Do Work (atomic operations)
Thread 2: Do Work (atomic operations)  ← No blocking!
```
Multiple threads work simultaneously.

**Benefits:**
- **No waiting** - threads never block each other
- **Lower overhead** - eliminates mutex lock/unlock operations
- **Scalable** - performance improves with more cores

---

## Integrated Optimizations

### What Was Implemented

1. **Lock-Free Message Pools**
   - TLS pools have no mutex
   - Zero contention on hot path
   - Automatic per-thread allocation

2. **Lock-Free Mailboxes**
   - SPSC (Single Producer Single Consumer) atomic queue
   - 64-byte cache line padding
   - Runtime switchable

3. **Runtime Configuration API**
   - Control optimizations via flags
   - Auto-detect CPU features
   - Enabled by default without manual configuration

---

## Usage

### Option 1: Auto-Detect (Recommended)
```c
#include "runtime/aether_runtime.h"

int main() {
    // One line - automatically enables best optimizations
    aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);
    
    // Your code here...
    
    aether_runtime_shutdown();
}
```

**Example output on a modern CPU:**
- Lock-free mailboxes: ENABLED
- Lock-free pools: ENABLED
- MWAIT idle: ENABLED
- AVX2 SIMD: ENABLED

---

### Option 2: Manual Control
```c
// Explicit flags - full control
aether_runtime_init(8,
    AETHER_FLAG_LOCKFREE_MAILBOX |  // Use lock-free mailboxes
    AETHER_FLAG_LOCKFREE_POOLS   |  // Use lock-free TLS pools
    AETHER_FLAG_ENABLE_SIMD      |  // Use AVX2 vectorization
    AETHER_FLAG_ENABLE_MWAIT);      // Use MWAIT for idle
```

---

### Option 3: Compatibility Mode
```c
// No optimizations - works on any CPU
aether_runtime_init(4, 0);  // 4 cores, no flags
```

---

### Option 4: Debug/Verbose
```c
// Print configuration on startup
aether_runtime_init(0, 
    AETHER_FLAG_AUTO_DETECT | 
    AETHER_FLAG_VERBOSE);

// Output:
// ========================================
//   Aether Runtime Configuration
// ========================================
// CPU: <detected processor>
// Active Optimizations:
//   Lock-free mailbox: ENABLED
//   Lock-free pools:   ENABLED
//   ...
```

---

## Available Flags

| Flag | Hex | Description |
|------|-----|-------------|
| `AETHER_FLAG_AUTO_DETECT` | 0x01 | Auto-detect CPU features (recommended) |
| `AETHER_FLAG_LOCKFREE_MAILBOX` | 0x02 | Use lock-free SPSC mailboxes |
| `AETHER_FLAG_LOCKFREE_POOLS` | 0x04 | Use lock-free TLS message pools |
| `AETHER_FLAG_ENABLE_SIMD` | 0x08 | Enable AVX2 vectorization |
| `AETHER_FLAG_ENABLE_MWAIT` | 0x10 | Enable MWAIT idle strategy |
| `AETHER_FLAG_VERBOSE` | 0x20 | Print configuration on init |

**Combine flags with bitwise OR:**
```c
int flags = AETHER_FLAG_LOCKFREE_MAILBOX | 
            AETHER_FLAG_LOCKFREE_POOLS;
aether_runtime_init(8, flags);
```

---

## Performance Expectations

Performance varies by hardware and workload. Use the profiling tools to measure throughput on your target platform.

See [benchmarks/cross-language](../benchmarks/cross-language/) for detailed measurements.

---

## Examples

### Example 1: Simple Program
Actors are produced by the Aether compiler: an `actor` block becomes a struct
carrying a mailbox, and the runtime flags below decide which mailbox and pool
implementations it uses. A C program embedding the runtime configures it and
then drives compiler-generated actors, or its own structs built on the mailbox
API in `runtime/actors/actor_state_machine.h`.

```c
#include "runtime/aether_runtime.h"
#include "runtime/actors/actor_state_machine.h"

typedef struct Counter {
    int id;
    atomic_int active;
    Mailbox mailbox;
    int count;
} Counter;

int main() {
    // Initialize with auto-detect
    aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);

    Counter c = {0};
    mailbox_init(&c.mailbox);

    Message msg = {1, 0, 42, NULL};
    mailbox_send(&c.mailbox, msg);
    if (mailbox_receive(&c.mailbox, &msg)) {
        c.count++;
    }

    aether_runtime_shutdown();
    return 0;
}
```

### Example 2: Query Configuration
```c
const AetherRuntimeInitConfig* config = aether_runtime_get_config();

if (config->use_lockfree_mailbox) {
    printf("Using lock-free mailboxes!\n");
}

if (config->use_simd) {
    printf("AVX2 SIMD enabled\n");
}
```

### Example 3: Feature Check
```c
if (aether_runtime_has_feature(AETHER_FLAG_ENABLE_MWAIT)) {
    printf("MWAIT is enabled\n");
}
```

---

## Testing

### Check CPU Features

Use the verbose flag to see detected CPU features at startup:
```c
aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT | AETHER_FLAG_VERBOSE);
```

### Run Configuration Example

Build and run the C example under `runtime/examples/`:
```bash
cd runtime/examples
gcc -O2 -o runtime_config runtime_config_example.c ../../runtime/*.c -I../..
./runtime_config
```

---

## How It Works Internally

### Mailbox Selection
```c
// In aether_actor_create()
Actor* actor = aether_actor_create(process_fn);

// Runtime automatically selects mailbox type:
if (runtime_config->use_lockfree_mailbox) {
    // Use atomic SPSC queue (no mutex)
    lockfree_mailbox_init(&actor->mailbox.lockfree);
} else {
    // Use simple ring buffer
    mailbox_init(&actor->mailbox.simple);
}
```

### Message Send
```c
// In aether_send_message()
if (actor->use_lockfree) {
    lockfree_mailbox_send(&actor->mailbox.lockfree, msg);  // Atomic
} else {
    mailbox_send(&actor->mailbox.simple, msg);  // Simple
}
```

### Message Pool Allocation
```c
// In message_pool_alloc()
if (pool->is_thread_local) {
    // LOCK-FREE PATH (no mutex)
    buffer = pool->buffers[pool->head];
    pool->head = (pool->head + 1) % MESSAGE_POOL_SIZE;
    return buffer;
} else {
    // Shared pool (with mutex)
    pthread_mutex_lock(&pool->lock);
    // ...
}
```

---

## Environment Variables

| Variable | Effect |
|----------|--------|
| `AETHER_NO_INLINE` | Disables Main Thread Actor Mode (forces scheduler-based processing) |
| `AETHER_INLINE` | Forces Main Thread Actor Mode even with multiple actors |
| `AETHER_SINGLE_CORE` | Pins all actors to core 0 (eliminates cross-core overhead) |
| `AETHER_PROFILE` | Memory profile: `micro`, `small`, `medium` (default), `large` |
| `AETHER_MSG_POOL_SIZE` | Override message pool size (default from profile) |
| `AETHER_VERBOSE` | Print optimization configuration at startup |
| `AETHER_PREEMPT` | Enable cooperative preemption (time-based drain loop break, default 1ms) |
| `AETHER_PREEMPT_MS` | Override preemption threshold in milliseconds (default 1) |
| `BENCHMARK_MESSAGES` | Sets message count for benchmark programs |

---

## Best Practices

1. **Always use `AETHER_FLAG_AUTO_DETECT`** unless you have a specific reason not to
2. **Use `AETHER_FLAG_VERBOSE`** during development to see what's enabled
3. **Don't recompile** for different CPUs - auto-detect handles it
4. **Test on target hardware** to verify optimizations are available
5. **Profile first** - measure actual performance gains

---

## Cross-Platform Support

| Platform | Lock-Free | MWAIT | SIMD | Thread Affinity | Status |
|----------|-----------|-------|------|-----------------|--------|
| **Intel x86** (2013+) | Yes | Yes | AVX2 | Hard | Full support |
| **AMD Ryzen** | Yes | Yes | AVX2 | Hard | Full support |
| **Apple Silicon** (M1/M2/M3) | Yes | No (WFE) | NEON | Advisory | P-cores only |
| **Old x86** (pre-2013) | Yes | No | SSE4.2 | Hard | Partial |
| **ARM Linux** | Yes | No (WFE) | NEON | Hard | Full support |
| **Windows** | Yes | Yes | AVX2 | Hard | Full support |
| **WebAssembly** (Emscripten) | N/A | No | No | No | Cooperative scheduler |
| **Embedded** (ARM bare-metal) | N/A | No | No | No | Cooperative scheduler |

With `AETHER_FLAG_AUTO_DETECT`, the runtime enables the optimizations each platform supports.

### Cooperative Mode

On platforms without pthreads (WASM, embedded), or when threading is explicitly disabled (`-DAETHER_NO_THREADING`), the cooperative scheduler handles all actor processing on a single thread. Multi-actor programs work correctly, messages are processed cooperatively during `wait_for_idle()`.

```bash
# Force cooperative mode on native (for testing):
make stdlib EXTRA_CFLAGS="-DAETHER_NO_THREADING"
ae run examples/actors/counter.ae

# Build for WebAssembly:
make stdlib PLATFORM=wasm
```

### Platform Capability Flags

All platform-dependent code is gated behind `AETHER_HAS_*` compile-time flags defined in `runtime/config/aether_optimization_config.h`. These are auto-detected but can be overridden:

```bash
# Disable specific features
make stdlib EXTRA_CFLAGS="-DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING"

# Query capabilities at runtime
AETHER_VERBOSE=1 ./program
```

**Apple Silicon Notes:**
- P-cores (Performance) detected via `hw.perflevel0.physicalcpu`
- E-cores (Efficiency) excluded for consistent throughput
- QoS hints encourage P-core scheduling
- Thread affinity is advisory; macOS may migrate threads

---

## Troubleshooting

### Issue: "Optimization not enabled"
**Solution:** Use `AETHER_FLAG_VERBOSE` to see why:
```c
aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT | AETHER_FLAG_VERBOSE);
```

### Issue: "Performance not as expected"
**Check:**
1. CPU actually supports features (use `AETHER_FLAG_VERBOSE` to see detected capabilities)
2. Flags are set correctly
3. Actors created after `aether_runtime_init()`

### Issue: "Works on development machine, not on server"
**Reason:** Different CPU capabilities
**Solution:** Use `AETHER_FLAG_AUTO_DETECT` - it adapts automatically
