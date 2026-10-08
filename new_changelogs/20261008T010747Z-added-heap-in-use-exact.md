- **`mem.heap_in_use_exact()` says whether `heap_in_use()` counts exactly the
  blocks the program holds.** It is true on Windows (a walk of the process's
  heaps) and under a sanitizer's allocator, where any growth between steady
  rounds is a leak. glibc, macOS and FreeBSD report from allocator statistics
  that also count freed blocks parked in per-thread caches; on a workload that
  churns many allocations those settle over many rounds, so two rounds there
  can differ by kilobytes without a leak (glibc and macOS x86_64 both showed
  it in CI while the same programs grew by zero on Windows). The `std.mem`
  documentation no longer calls the statistics exact everywhere, and the
  regression tests that check heap growth run that check only where the count
  is exact, leaving the other platforms to the leaks gate.
