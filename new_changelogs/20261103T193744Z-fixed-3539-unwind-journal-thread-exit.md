- **A thread that ran Aether code no longer leaks its unwind journal when it
  ends.** Only the scheduler's threads gave back the per-thread allocation
  journal and teardown queue. A host thread calling into an `--emit=lib`
  library, a `std.worker` thread or an HTTP server pool thread left both
  behind, about 1 to 3 KB per thread. The first growth of either now sets a
  thread-exit hook that frees them.
