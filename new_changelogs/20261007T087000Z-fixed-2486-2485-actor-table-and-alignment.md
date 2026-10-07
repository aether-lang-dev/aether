- **A scheduler core's actor table is safe to read while it grows (#2486).**
  Each core's thread scans its actor table without a lock, and the main
  thread walks every table in `aether_scheduler_poll()` and
  `scheduler_wait()`, while other threads register, migrate and steal
  actors under the core's lock. The reads were plain loads behind a fence
  that ordered nothing, so a reader could pair the count of a grown table
  with the pointer of the old one and read past its end, and a grown table
  kept malloc's garbage past the copied actors. Now the count, the table
  pointer and every slot are atomic: a writer publishes a slot and the
  table before the count (release), a reader loads the count before the
  table (acquire), so the count never exceeds the table it is used with.
  A grown table is zeroed past its copied prefix, a removal clears the slot
  it vacates, and a replaced table stays allocated until
  `scheduler_cleanup()`, which now frees every one with its own size. A
  migration from the idle scan finds the actor in the current table instead
  of trusting the index it was seen at, which a steal can have moved.
- **Actors are allocated on the 64-byte boundary their structs are declared
  with (#2485).** Generated actor structs carry `aligned(64)`, but
  `scheduler_spawn_actor` allocated them with plain `malloc` (16 bytes) in
  every build without libnuma, so most actors were misaligned: undefined
  behaviour, and no cache-line isolation. Actors now come from
  `aether_numa_alloc_aligned` (`_aligned_malloc` on Windows,
  `posix_memalign` elsewhere, a page-aligned NUMA mapping when libnuma is
  in use) and go back through the matching `aether_numa_free_aligned`, in
  the threaded and the cooperative scheduler alike. The generated spawn
  function no longer retries a failed spawn with `aligned_alloc`, whose
  block `scheduler_release_actor` could not free correctly on Windows.
