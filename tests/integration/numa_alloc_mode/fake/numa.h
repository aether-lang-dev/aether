/* Stand-in for libnuma's <numa.h> (#3680): the test defines these, so the
 * NUMA allocation path builds and runs on a host with one node, or none. */
#ifndef AETHER_TEST_FAKE_NUMA_H
#define AETHER_TEST_FAKE_NUMA_H
#include <stddef.h>
int numa_available(void);
int numa_num_configured_nodes(void);
int numa_node_of_cpu(int cpu);
void* numa_alloc_onnode(size_t size, int node);
void* numa_alloc_local(size_t size);
void numa_free(void* start, size_t size);
#endif
