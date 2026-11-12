/* A block is freed by the allocator that made it, whatever the NUMA state
 * at free time (#3680).
 *
 * A scheduler's shutdown calls aether_numa_cleanup right after joining its
 * threads, and the core tables are freed after that (scheduler_cleanup, or
 * the next scheduler_init). When the free asked the topology's once whether
 * NUMA was on, the cleanup's reset sent every NUMA block to the heap's free:
 * glibc aborts, Windows corrupts its heap. Only a host with more than one
 * node took the NUMA path, so this test builds the module with its NUMA path
 * on (AETHER_HAS_NUMA=1, libnuma faked on POSIX) and two mocked nodes, and
 * tracks every block the mocked NUMA allocator hands out: a NUMA block that
 * reaches the heap's free, or a heap block that reaches the NUMA free, is a
 * failure. It runs on any host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#endif

#define MAX_BLOCKS 64
static void* numa_blocks[MAX_BLOCKS];
static int numa_live = 0;
static int heap_freed_numa = 0;   /* a NUMA block given to the heap's free */
static int numa_freed_heap = 0;   /* a heap block given to the NUMA free */

static int numa_block_index(void* p) {
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (p && numa_blocks[i] == p) return i;
    }
    return -1;
}

/* The NUMA allocator's blocks come from the page-aligned heap, kept apart
 * from the module's own heap frees below so a mix-up is counted, not run. */
static void* mock_numa_block(size_t size) {
    int slot = -1;
    for (int i = 0; i < MAX_BLOCKS && slot < 0; i++) {
        if (!numa_blocks[i]) slot = i;
    }
    if (slot < 0) return NULL;
#ifdef _WIN32
    void* p = _aligned_malloc(size, 4096);
#else
    void* p = NULL;
    if (posix_memalign(&p, 4096, size) != 0) p = NULL;
#endif
    if (p) {
        numa_blocks[slot] = p;
        numa_live++;
    }
    return p;
}

static int mock_numa_release(void* p) {
    int i = numa_block_index(p);
    if (i < 0) return 0;
    numa_blocks[i] = NULL;
    numa_live--;
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
    return 1;
}

/* The module's heap frees: a NUMA block that reaches one is counted and
 * kept (it is the mock's to release). */
static void test_free(void* p) {
    if (numa_block_index(p) >= 0) { heap_freed_numa++; return; }
    (free)(p);
}

#ifdef _WIN32
static void test_aligned_free(void* p) {
    if (numa_block_index(p) >= 0) { heap_freed_numa++; return; }
    (_aligned_free)(p);
}

/* Two nodes; each CPU on node (cpu % 2). */
static BOOL mock_highest_node(PULONG highest) { *highest = 1; return TRUE; }
static BOOL mock_processor_node(PPROCESSOR_NUMBER proc, PUSHORT node) {
    *node = (USHORT)(proc->Number % 2);
    return TRUE;
}
static LPVOID mock_virtual_alloc_numa(HANDLE process, LPVOID addr, SIZE_T size,
                                      DWORD type, DWORD protect, DWORD node) {
    (void)process; (void)addr; (void)type; (void)protect; (void)node;
    return mock_numa_block(size);
}
/* As VirtualFree: refuses what is not the base of one of its regions. */
static BOOL mock_virtual_free(LPVOID p, SIZE_T size, DWORD type) {
    (void)size; (void)type;
    return mock_numa_release(p) ? TRUE : FALSE;
}
#define GetNumaHighestNodeNumber mock_highest_node
#define GetNumaProcessorNodeEx mock_processor_node
#define VirtualAllocExNuma mock_virtual_alloc_numa
#define VirtualFree mock_virtual_free
#define _aligned_free(p) test_aligned_free(p)
#else
#define HAVE_LIBNUMA 1
int numa_available(void) { return 0; }
int numa_num_configured_nodes(void) { return 2; }
int numa_node_of_cpu(int cpu) { return cpu % 2; }
void* numa_alloc_onnode(size_t size, int node) { (void)node; return mock_numa_block(size); }
void* numa_alloc_local(size_t size) { return mock_numa_block(size); }
void numa_free(void* p, size_t size) {
    (void)size;
    if (!mock_numa_release(p)) numa_freed_heap++;
}
#endif
#define free(p) test_free(p)

#include "../../../runtime/aether_numa.c"

#undef free

static int fails = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s (NUMA blocks live %d, freed by the heap %d; heap blocks freed by NUMA %d)\n",
               what, numa_live, heap_freed_numa, numa_freed_heap);
        fails++;
    }
}

int main(void) {
    /* A block made before the first scheduler starts: the allocation mode
     * is decided then, by the same detection, and kept. */
    void* early = aether_numa_alloc_aligned(256, 64, 1);
    check(early != NULL, "a block before the first init");

    aether_numa_topology_t topo = aether_numa_init();
    check(topo.available && topo.num_nodes == 2, "the mocked topology is two nodes");
    check(aether_numa_node_of_cpu(0) == 0, "cpu 0 is on node 0");

    /* What a scheduler makes: an actor (aligned) and an actor table. */
    void* actor = aether_numa_alloc_aligned(256, 64, 1);
    void* table = aether_numa_alloc(128, 0);
    check(actor != NULL && table != NULL, "a scheduler's blocks");
    check(numa_block_index(actor) >= 0 && numa_block_index(table) >= 0,
          "with NUMA on, the blocks come from the NUMA allocator");

    /* scheduler_shutdown: the cleanup, then scheduler_cleanup's frees. */
    aether_numa_cleanup();
    check(aether_numa_node_of_cpu(0) == -1, "the map is released by the cleanup");
    aether_numa_free_aligned(actor, 256);
    aether_numa_free(table, 128);
    aether_numa_free_aligned(early, 256);
    check(heap_freed_numa == 0, "no NUMA block goes to the heap after the cleanup");
    check(numa_freed_heap == 0, "no heap block goes to the NUMA free");
    check(numa_live == 0, "every NUMA block is released by the NUMA allocator");

    /* A restart: the topology is detected again and the blocks match. */
    topo = aether_numa_init();
    check(topo.available && aether_numa_node_of_cpu(0) == 0, "a restart detects the map again");
    void* again = aether_numa_alloc_aligned(512, 64, 0);
    aether_numa_cleanup();
    aether_numa_free_aligned(again, 512);
    check(heap_freed_numa == 0 && numa_freed_heap == 0 && numa_live == 0,
          "a restarted scheduler's blocks are freed by their allocator too");

    if (fails) return 1;
    puts("PASS: NUMA blocks are freed by the NUMA allocator before and after the cleanup");
    return 0;
}
