- **`mem.heap_in_use_exact()` says whether `heap_in_use()` counts exactly
  the blocks the program holds.** It is true on Windows (a walk of the
  process's heaps) and under a sanitizer's allocator, where any growth
  between steady rounds is a leak. glibc, macOS and FreeBSD report allocator
  statistics that include freed blocks parked in per-thread caches, so two
  steady rounds there can differ by kilobytes without a leak; the `std.mem`
  documentation no longer calls those statistics exact.
