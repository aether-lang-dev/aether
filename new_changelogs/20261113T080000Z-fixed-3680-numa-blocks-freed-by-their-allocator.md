- **Runtime: a NUMA block is always freed by the allocator that made it
  (#3680).** Whether the runtime allocates on NUMA nodes is decided once per
  process, before its first block, and no longer read from the topology,
  which a scheduler's shutdown resets before the core tables are freed. On a
  host with more than one node, those frees after the shutdown (and a
  restarted scheduler's first one) sent NUMA blocks to the heap's free, which
  aborts on glibc and corrupts the heap on Windows. A build with libnuma also
  no longer gives `numa_free` a heap block made before the first scheduler
  started. Covered by `tests/integration/numa_alloc_mode`, which mocks two
  nodes so it runs on any host.
