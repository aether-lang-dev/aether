#ifndef AETHER_NUMA_H
#define AETHER_NUMA_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NUMA (Non-Uniform Memory Access) support for Aether runtime.
 * 
 * Provides topology detection and NUMA-aware memory allocation to minimize
 * cross-node memory access latency in multicore systems.
 */

typedef struct {
    int num_nodes;              // Number of NUMA nodes
    int num_cpus;               // Total number of CPUs
    int *cpu_to_node;           // Map: cpu_id -> numa_node_id
    bool available;             // Whether NUMA is available on this system
} aether_numa_topology_t;

/**
 * Initialize NUMA subsystem and detect topology.
 * Returns topology information.
 */
aether_numa_topology_t aether_numa_init(void);

/**
 * Get the NUMA node for a given CPU core.
 * Returns -1 if NUMA is not available.
 */
int aether_numa_node_of_cpu(int cpu_id);

/**
 * Allocate memory on a specific NUMA node.
 * Falls back to regular malloc if NUMA is not available.
 * 
 * @param size Size in bytes to allocate
 * @param node NUMA node to allocate on (-1 for any node)
 * @return Pointer to allocated memory, or NULL on failure
 */
void* aether_numa_alloc(size_t size, int node);

/**
 * Free memory allocated with aether_numa_alloc.
 */
void aether_numa_free(void* ptr, size_t size);

/**
 * Allocate memory on a NUMA node, aligned to `align` bytes (#2485).
 * aether_numa_alloc only promises malloc's alignment when it falls back to
 * the heap, which is 16 bytes, too little for a type declared aligned(64).
 *
 * @param size  Size in bytes to allocate
 * @param align Alignment: a power of two, a multiple of sizeof(void*), and
 *              at most 4096 (a NUMA mapping is only page-aligned)
 * @param node  NUMA node to allocate on (-1 for any node)
 * @return Pointer to allocated memory, or NULL on failure
 */
void* aether_numa_alloc_aligned(size_t size, size_t align, int node);

/**
 * Free memory allocated with aether_numa_alloc_aligned, and only that: the
 * heap fallback comes from an aligned allocator (_aligned_malloc on Windows)
 * whose blocks free() must not see.
 */
void aether_numa_free_aligned(void* ptr, size_t size);

/**
 * Cleanup NUMA subsystem.
 */
void aether_numa_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // AETHER_NUMA_H
