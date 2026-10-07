#include "aether_numa.h"
#include "config/aether_optimization_config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <malloc.h>
#endif

/* The heap half of aether_numa_alloc_aligned (#2485). malloc only promises
 * 16 bytes, so an aligned(64) actor needs an aligned allocator, and each one
 * has its own free: _aligned_free on Windows, plain free after
 * posix_memalign. Every build that has no NUMA mapping to hand out comes
 * through here. */
static void* aligned_heap_alloc(size_t size, size_t align) {
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    void* ptr = NULL;
    return posix_memalign(&ptr, align, size) == 0 ? ptr : NULL;
#endif
}

static void aligned_heap_free(void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

#if !AETHER_HAS_NUMA
// Minimal stubs when NUMA is disabled (embedded, WASM, macOS, or -DAETHER_NO_NUMA)

static aether_numa_topology_t g_topology = {0};
static bool g_initialized = false;

aether_numa_topology_t aether_numa_init(void) {
    if (g_initialized) return g_topology;
    g_topology.num_nodes = 1;
    g_topology.num_cpus = 1;
    g_topology.cpu_to_node = NULL;
    g_topology.available = false;
    g_initialized = true;
    return g_topology;
}

int aether_numa_node_of_cpu(int cpu_id) { (void)cpu_id; return -1; }
void* aether_numa_alloc(size_t size, int node) { (void)node; return malloc(size); }
void aether_numa_free(void* ptr, size_t size) { (void)size; free(ptr); }
void* aether_numa_alloc_aligned(size_t size, size_t align, int node) {
    (void)node;
    return aligned_heap_alloc(size, align);
}
void aether_numa_free_aligned(void* ptr, size_t size) { (void)size; aligned_heap_free(ptr); }
void aether_numa_cleanup(void) { g_initialized = false; }

#else
// Full NUMA implementation

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sched.h>
// Try to use libnuma if available
#ifdef HAVE_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif
#endif

static aether_numa_topology_t g_topology = {0};
static bool g_initialized = false;

#ifdef _WIN32

aether_numa_topology_t aether_numa_init(void) {
    if (g_initialized) {
        return g_topology;
    }

    aether_numa_topology_t topo = {0};
    topo.available = false;

    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    topo.num_cpus = sysinfo.dwNumberOfProcessors;

    // Get NUMA node count
    ULONG highest_node = 0;
    if (GetNumaHighestNodeNumber(&highest_node)) {
        topo.num_nodes = highest_node + 1;
        topo.available = (topo.num_nodes > 1);
    } else {
        topo.num_nodes = 1;
    }

    // Allocate CPU to node mapping
    topo.cpu_to_node = (int*)malloc(sizeof(int) * topo.num_cpus);
    if (!topo.cpu_to_node) {
        return topo;
    }

    // Map each CPU to its NUMA node
    for (int cpu = 0; cpu < topo.num_cpus; cpu++) {
        USHORT node_number = 0;
        PROCESSOR_NUMBER proc_num;
        proc_num.Group = 0;
        proc_num.Number = cpu;
        proc_num.Reserved = 0;

        if (GetNumaProcessorNodeEx(&proc_num, &node_number)) {
            topo.cpu_to_node[cpu] = node_number;
        } else {
            topo.cpu_to_node[cpu] = 0;
        }
    }

    g_topology = topo;
    g_initialized = true;
    return topo;
}

int aether_numa_node_of_cpu(int cpu_id) {
    if (!g_initialized || !g_topology.available || cpu_id < 0 || cpu_id >= g_topology.num_cpus) {
        return -1;
    }
    return g_topology.cpu_to_node[cpu_id];
}

void* aether_numa_alloc(size_t size, int node) {
    if (!g_topology.available || node < 0 || node >= g_topology.num_nodes) {
        return malloc(size);
    }

    // Use VirtualAllocExNuma for NUMA-aware allocation
    void* ptr = VirtualAllocExNuma(
        GetCurrentProcess(),
        NULL,
        size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE,
        node
    );

    if (!ptr) {
        // Fallback to regular allocation
        return malloc(size);
    }

    return ptr;
}

void aether_numa_free(void* ptr, size_t size) {
    if (!ptr) return;

    if (!g_topology.available) {
        free(ptr);
        return;
    }

    if (!VirtualFree(ptr, 0, MEM_RELEASE)) {
        free(ptr);
    }
}

void* aether_numa_alloc_aligned(size_t size, size_t align, int node) {
    if (g_topology.available && node >= 0 && node < g_topology.num_nodes) {
        // A VirtualAlloc region starts on the 64 KiB allocation granularity,
        // which covers any alignment this function accepts.
        void* ptr = VirtualAllocExNuma(GetCurrentProcess(), NULL, size,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, node);
        if (ptr) return ptr;
    }
    return aligned_heap_alloc(size, align);
}

void aether_numa_free_aligned(void* ptr, size_t size) {
    (void)size;
    if (!ptr) return;
    // As in aether_numa_free: only a VirtualAllocExNuma block is the base of a
    // region, so VirtualFree releases those and refuses a heap block, which
    // came from _aligned_malloc and goes back through _aligned_free.
    if (g_topology.available && VirtualFree(ptr, 0, MEM_RELEASE)) return;
    aligned_heap_free(ptr);
}

#else // Linux/Unix

aether_numa_topology_t aether_numa_init(void) {
    if (g_initialized) {
        return g_topology;
    }

    aether_numa_topology_t topo = {0};
    topo.available = false;

    topo.num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

#ifdef HAVE_LIBNUMA
    // Check if NUMA is available
    if (numa_available() >= 0) {
        topo.num_nodes = numa_num_configured_nodes();
        topo.available = (topo.num_nodes > 1);

        // Allocate CPU to node mapping
        topo.cpu_to_node = (int*)malloc(sizeof(int) * topo.num_cpus);
        if (topo.cpu_to_node) {
            for (int cpu = 0; cpu < topo.num_cpus; cpu++) {
                topo.cpu_to_node[cpu] = numa_node_of_cpu(cpu);
            }
        } else {
            // Couldn't build the CPU->node map: disable NUMA rather than report
            // it available with a NULL map that aether_numa_node_of_cpu would
            // dereference. Falls back to single-node (plain malloc) allocation.
            topo.available = false;
            topo.num_nodes = 1;
        }
    } else {
        topo.num_nodes = 1;
    }
#else
    // NUMA not available - single node
    topo.num_nodes = 1;
    topo.cpu_to_node = (int*)malloc(sizeof(int) * topo.num_cpus);
    if (topo.cpu_to_node) {
        for (int cpu = 0; cpu < topo.num_cpus; cpu++) {
            topo.cpu_to_node[cpu] = 0;
        }
    }
#endif

    g_topology = topo;
    g_initialized = true;
    return topo;
}

int aether_numa_node_of_cpu(int cpu_id) {
    if (!g_initialized || !g_topology.available || cpu_id < 0 || cpu_id >= g_topology.num_cpus) {
        return -1;
    }
    return g_topology.cpu_to_node[cpu_id];
}

void* aether_numa_alloc(size_t size, int node) {
#ifdef HAVE_LIBNUMA
    if (g_topology.available) {
        // Always use NUMA allocator when available so aether_numa_free (which
        // calls numa_free unconditionally) always receives a NUMA-allocated ptr.
        if (node >= 0 && node < g_topology.num_nodes) {
            return numa_alloc_onnode(size, node);
        }
        return numa_alloc_local(size);  // fallback: local node, not malloc()
    }
#endif
    return malloc(size);
}

void aether_numa_free(void* ptr, size_t size) {
    if (!ptr) return;

#ifdef HAVE_LIBNUMA
    if (g_topology.available) {
        numa_free(ptr, size);
        return;
    }
#endif
    free(ptr);
}

void* aether_numa_alloc_aligned(size_t size, size_t align, int node) {
#ifdef HAVE_LIBNUMA
    // numa_alloc_* maps whole pages, so the block is page-aligned, which
    // covers any alignment this function accepts. The free below makes the
    // same choice on the same flag.
    if (g_topology.available) return aether_numa_alloc(size, node);
#else
    (void)node;
#endif
    return aligned_heap_alloc(size, align);
}

void aether_numa_free_aligned(void* ptr, size_t size) {
    if (!ptr) return;
#ifdef HAVE_LIBNUMA
    if (g_topology.available) {
        numa_free(ptr, size);
        return;
    }
#else
    (void)size;
#endif
    aligned_heap_free(ptr);
}

#endif

void aether_numa_cleanup(void) {
    if (g_initialized && g_topology.cpu_to_node) {
        free(g_topology.cpu_to_node);
        g_topology.cpu_to_node = NULL;
    }
    g_initialized = false;
}

#endif // AETHER_HAS_NUMA
