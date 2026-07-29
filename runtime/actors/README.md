# Actor Runtime Optimizations

This directory contains the core actor runtime with performance optimizations.

## Core Components

### actor_state_machine.h
Base actor implementation with lightweight message passing.

**Key Features:**
- Ring buffer mailboxes (power-of-2 for fast masking)
- Branch prediction hints for hot paths
- Batch message processing
- Zero-copy message transfer

### Performance Optimizations

#### 1. Direct Actor Bypass (aether_direct_send.h)
Skips mailbox for same-core actors by directly invoking message handlers.

**Implementation:**
- Thread-local scheduler tracking
- Same-core detection via actor metadata
- Direct function call replaces mailbox enqueue/dequeue
- Queue depth heuristic prevents overwhelming busy actors

**Use Case:** Request-response patterns where actors communicate primarily within a core.

**Expected Improvement:** Eliminates mailbox latency for intra-core communication.

#### 2. Message Deduplication (aether_message_dedup.h)
Detects and skips redundant messages using a rolling window.

**Implementation:**
- 16-message rolling window of fingerprints
- Fast hash-based fingerprinting (Knuth multiplicative)
- O(1) duplicate detection with 16-slot search
- Automatic eviction of oldest entries

**Use Case:** Workloads with redundant state updates or repeated notifications.

**Expected Improvement:** Reduces processing overhead for duplicate-heavy patterns.

#### 3. Adaptive Batch Processing (aether_adaptive_batch.h)
Dynamically adjusts batch size based on message queue utilization.

**Implementation:**
- Tracks consecutive full/partial batches
- Increases batch size (4→8→16→32→64) when consistently full
- Decreases batch size when consistently partial
- Bounded by MIN_BATCH_SIZE (4) and MAX_BATCH_SIZE (64)

**Use Case:** Variable-load scenarios where message arrival rate fluctuates.

**Expected Improvement:** Balances throughput and latency based on load.

## Usage

### Direct Send Optimization

```c
#include "aether_direct_send.h"

// Set current scheduler
current_scheduler_id = 0;

// Try direct send
if (!direct_send(&sender_meta, &receiver_meta, msg)) {
    // Fall back to normal mailbox send
    mailbox_send(&receiver->mailbox, msg);
}
```

### Message Deduplication

```c
#include "aether_message_dedup.h"

DedupWindow window = {0};

// Check before sending
if (!is_duplicate(&window, &msg)) {
    mailbox_send(&actor->mailbox, msg);
    record_message(&window, &msg);
}
```

### Adaptive Batching

```c
#include "aether_adaptive_batch.h"

AdaptiveBatchState state;
adaptive_batch_init(&state);

// Receive with adaptive batch size
Message msgs[64];
int count = adaptive_batch_receive(&state, &mbox, msgs, 64);
```

## Testing

The runtime C test suite covers these components:

```bash
make test
```

## Performance Notes

- All optimizations are optional and can be used independently or combined
- Benchmarks should be run on target hardware to validate improvements
- Some optimizations benefit specific workload patterns more than others
- Profiling recommended to identify which optimizations apply to your use case

See [Runtime Optimizations](../../docs/runtime-optimizations.md) for the
full optimization catalog and [Actor Concurrency](../../docs/actor-concurrency.md)
for the scheduler design.
