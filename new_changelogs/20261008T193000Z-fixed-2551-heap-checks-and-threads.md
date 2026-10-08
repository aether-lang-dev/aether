- **A same-process leak check no longer counts a thread's own blocks as a
  leak (#2551).** While a thread lives, the OS and the C runtime keep heap
  blocks for it (on Windows, 1088 bytes with msvcrt and 2264 with UCRT), so
  `mem.heap_in_use()` moved whenever any thread started: one of the
  program's, or one the system starts in a process, such as a thread-pool
  worker. Windows CI failed three exact heap checks this way (1048 bytes in
  each of two windows), each time in a different test. `mem.thread_epoch()`
  changes whenever a thread starts or ends. On Windows it counts the
  loader's thread notifications through a TLS callback, so it sees threads
  no Aether code created; on Linux, macOS and FreeBSD it is the thread
  count. `mem.steady_growth(round, tries)` runs a round of work up to `tries`
  times, each run between two reads. It compares only runs in which no
  thread started or ended, and returns 0 as soon as one leaves the count
  exactly as it found it. A leak grows every run; a one-time allocation, or
  a buffer still growing to its high-water mark (the scheduler's per-core
  overflow buffers under a slow runner), settles. The repository's exact
  heap checks use it.
