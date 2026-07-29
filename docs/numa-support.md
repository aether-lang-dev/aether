# NUMA Support in Aether

## Overview

Aether's multicore scheduler now includes NUMA (Non-Uniform Memory Access) awareness to minimize memory access latency on multi-socket and multi-die systems.

## What is NUMA?

In NUMA systems, memory is physically distributed across multiple nodes, each connected to specific CPU cores. Accessing "local" memory (on the same node) is faster than "remote" memory (on another node).

**Example NUMA System:**
```
┌─────────────────┐       ┌─────────────────┐
│  NUMA Node 0    │       │  NUMA Node 1    │
│  CPUs: 0-11     │       │  CPUs: 12-23    │
│  Local Memory   │◄─────►│  Local Memory   │
└─────────────────┘ slow  └─────────────────┘
       fast ↕                    fast ↕
```

## Implementation

### Architecture

1. **Topology Detection** (`runtime/aether_numa.h/c`)
   - Detects number of NUMA nodes
   - Maps each CPU to its NUMA node
   - Gracefully handles UMA (single-node) systems

2. **NUMA-Aware Allocation**
   - Actors allocated on same NUMA node as assigned core
   - Actor arrays allocated on core's NUMA node
   - Pool structures allocated on core's NUMA node

   These allocations route through `aether_numa_alloc(size, node)`. On a UMA system, or any build without libnuma detecting multiple nodes, `aether_numa_node_of_cpu` returns -1 and the call falls back to plain `malloc`. Binding actors to the cores whose node they were allocated on is a separate step, listed under Future Enhancements below.

3. **Platform Support**
   - **Windows**: `VirtualAllocExNuma`, `GetNumaProcessorNodeEx`
   - **Linux**: `numa_alloc_onnode` (requires libnuma)
   - **macOS**: Fallback to `malloc` (macOS has UMA memory on all current hardware)
   - **WASM/Embedded**: Stubs return `available = false` and delegate to `malloc` (gated by `AETHER_HAS_NUMA`)
   - **Fallback**: Regular `malloc` on UMA systems or when `-DAETHER_NO_NUMA` is set

### API

```c
// Initialize NUMA subsystem
aether_numa_topology_t aether_numa_init(void);

// Get NUMA node for CPU
int aether_numa_node_of_cpu(int cpu_id);

// Allocate on specific NUMA node
void* aether_numa_alloc(size_t size, int node);

// Free NUMA-allocated memory
void aether_numa_free(void* ptr, size_t size);

// Cleanup
void aether_numa_cleanup(void);
```

### Integration Points

Modified `runtime/scheduler/multicore_scheduler.c`:

1. **Scheduler Init**: Detects NUMA topology
2. **Actor Arrays**: Allocated on core's NUMA node
3. **Actor Instances**: Allocated on core's NUMA node
4. **Dynamic Growth**: NUMA-aware reallocation

## Performance Impact

### Expected Benefits on NUMA Systems

- **Reduced latency**: Local memory access typically has lower latency than remote access
- **Higher bandwidth**: Avoids contention on remote memory controllers
- **Better scaling**: NUMA-aware allocation improves scaling on multi-socket systems

### UMA Systems

- Zero overhead: Falls back to regular malloc
- No performance regression

### Testing

**Check NUMA Topology:**

On Linux, use `numactl` to check NUMA configuration:
```bash
numactl --hardware
```

On macOS, NUMA is not supported (single memory domain).

The runtime does not ship a topology-report tool. The following illustrate what the detected topology looks like on each kind of system.

**Illustrative topology (UMA system):**
```
NUMA Topology Detection
=======================

NUMA Available: No
Number of NUMA nodes: 1
Number of CPUs: 24

Single NUMA node (UMA) or NUMA not available
NUMA-aware allocation is disabled (fallback to malloc) in the multicore scheduler.
```

**Illustrative topology (NUMA system):**
```
NUMA Topology Detection
=======================

NUMA Available: Yes
Number of NUMA nodes: 2
Number of CPUs: 24

CPU to NUMA Node Mapping:
  CPU  0 -> NUMA Node 0
  CPU  1 -> NUMA Node 0
  ...
  CPU 12 -> NUMA Node 1
  CPU 13 -> NUMA Node 1
  ...

NUMA-aware allocation is active in the multicore scheduler.
```

## Build Integration

No changes needed. NUMA support is automatically included:

```bash
make test     # Tests include NUMA-aware scheduler
make examples # Benchmarks use NUMA allocation
```

## Linux Build Note

For full NUMA support on Linux, install libnuma:

```bash
# Ubuntu/Debian
sudo apt-get install libnuma-dev

# Build with NUMA
make EXTRA_CFLAGS="-DHAVE_LIBNUMA" LDFLAGS="-lm -pthread -lnuma"
```

`EXTRA_CFLAGS` is appended to the base compile flags. `LDFLAGS`, however, is a plain assignment in the Makefile, so setting it on the command line replaces the defaults; re-list `-lm -pthread` (plus any platform link libs your build needs) alongside `-lnuma`.

Without libnuma, the system gracefully falls back to malloc.

## Technical Details

### Windows Implementation

Maps CPUs to NUMA nodes within processor group 0 (`proc_num.Group = 0`), so it covers up to 64 logical processors. Systems that split cores across multiple processor groups (>64 logical processors) are not yet handled.
- `GetNumaHighestNodeNumber()` - Get node count
- `GetNumaProcessorNodeEx()` - Map CPU to node
- `VirtualAllocExNuma()` - Allocate on specific node
- `VirtualFree()` - Free NUMA allocation

### Linux Implementation

Uses libnuma when available:
- `numa_available()` - Check NUMA support
- `numa_num_configured_nodes()` - Get node count  
- `numa_node_of_cpu()` - Map CPU to node
- `numa_alloc_onnode()` - Allocate on specific node
- `numa_free()` - Free NUMA allocation

### Memory Management

**Allocation Strategy:**
- Static structures: Allocated at init on core's node
- Dynamic growth: Reallocated with NUMA awareness
- Free: Handles both NUMA and regular allocations

**Overhead:**
- Topology detection: Once at scheduler init
- Per-allocation: Node lookup from CPU ID (O(1) array access)

## Verification

All tests pass with NUMA support:
```bash
make test
```

The multicore benchmarks under `make examples` run through the NUMA-aware allocation path (they fall back to `malloc` unless a libnuma multi-node build is used):
```bash
make examples
```

## Future Enhancements

Potential improvements for true NUMA systems:

1. **NUMA-Aware Actor Placement**: Assign actors to cores on same node
2. **Cross-Node Messaging Optimization**: Different strategy for remote nodes
3. **Memory Binding**: Bind spawned threads to their NUMA nodes
4. **Topology-Aware Work Stealing**: Prefer stealing from same NUMA node

## References

- Windows NUMA: https://docs.microsoft.com/en-us/windows/win32/procthread/numa-support
- Linux NUMA: https://man7.org/linux/man-pages/man3/numa.3.html
- NUMA Architecture: https://en.wikipedia.org/wiki/Non-uniform_memory_access
