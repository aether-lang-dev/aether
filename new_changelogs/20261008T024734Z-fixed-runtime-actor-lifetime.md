- **A released actor is reclaimed once nothing can still hold it, and a
  late send to it is defined (#2509, #2517, #2527).**
  `scheduler_release_actor` freed the actor at once and left it in its core's
  table, so the lock-free table readers (the idle scan,
  `aether_scheduler_poll`, `scheduler_wait`) and any later send read freed
  memory. A release now marks the actor and takes it out of its table under
  the core's lock; the actor is reclaimed once every scheduler thread has
  passed the top of its loop since and every walk of the tables that was in
  progress has ended (each core publishes the epoch it last saw, a walker
  holds one for the walk, and a retired actor goes once every published
  epoch is past its own). Readers still take no lock. A release never blocks
  and is safe from the actor's own step: the thread running the step holds
  the actor until the step has returned and the thread reaches the top of
  its loop; one made while a main-thread-mode (inline) send is stepping an
  actor on that thread is carried out once the send is done with it, in the
  cooperative scheduler too, where it also waits for a poll or wait in
  progress. An actor with its own thread (`auto_process`) is only marked:
  the thread, which now publishes an epoch like a core (a send from its step
  to another actor of the core stepped that actor on the thread, which
  published none, so a release in that step freed the actor under it),
  leaves its loop at the mark, and whichever of the release and the thread's
  exit comes second ends the actor. A reclaimed block is not returned to the
  allocator while the scheduler runs: it stays marked released, in a bucket
  for its size, and the next spawn of that size takes it. A send that
  reaches it is dropped, counted (`scheduler_released_sends`) and reported
  once on stderr; one made after the block has become another actor reaches
  that actor (#2527). What the mailbox still held is released and counted as
  processed, so `scheduler_wait` does not wait for it. The cooperative
  scheduler drops such sends the same way; it used to deliver them.
- **`scheduler_deregister_actor` finds an actor that is migrating (#2509).**
  It read `assigned_core` before taking that core's lock, so an actor moved
  to another core in between was not removed. The core is now read again
  under the lock, and the lookup retries until it names the core held.
- **Main-thread mode is entered once per scheduler lifecycle (#2509).**
  Released actors bring the actor count back to zero, and the next spawn
  would have put that actor in main-thread mode, stepped inline on whichever
  thread spawned it, with the scheduler threads already running. Releasing
  the lone actor of main-thread mode now leaves the mode, which had kept
  pointing at the freed actor.
- **A scheduler core's actor table is safe to read while it grows (#2486).**
  Each core's thread scans its table without a lock, and the main thread
  walks every table in `aether_scheduler_poll()` and `scheduler_wait()`,
  while other threads register, migrate and steal actors under the core's
  lock. The reads were plain loads behind a fence that ordered nothing, so a
  reader could pair the count of a grown table with the pointer of the old
  one and read past its end, and a grown table kept malloc's garbage past
  the copied actors. The count, the table pointer and every slot are now
  atomic: a writer publishes a slot and the table before the count, a reader
  loads the count before the table, so the count never exceeds the table it
  is used with. A grown table is zeroed past its copied prefix, a removal
  clears the slot it vacates, and a replaced table stays allocated until
  `scheduler_cleanup()`. A migration from the idle scan finds the actor in
  the current table instead of trusting an index a steal can have moved.
- **Actors are allocated on the 64-byte boundary their structs are declared
  with (#2485).** Generated actor structs carry `aligned(64)`, but
  `scheduler_spawn_actor` used plain `malloc` (16 bytes) in every build
  without libnuma: undefined behaviour, and no cache-line isolation. Actors
  now come from `aether_numa_alloc_aligned` (`_aligned_malloc` on Windows,
  `posix_memalign` elsewhere, a page-aligned NUMA mapping when one is
  available) and go back through `aether_numa_free_aligned` at scheduler
  cleanup, in both schedulers. The generated spawn function no longer
  retries a failed spawn with `aligned_alloc`, whose block could not be
  freed correctly on Windows.
- **An HTTP server in actor dispatch mode releases its workers (#2509).** It
  spawned a worker actor per connection and never called the `release_fn`
  it was given, so each request left an actor allocated and registered.
  The worker now runs the server's own step, which runs the step it was
  given on the connection message and then releases the worker; a
  `spawn_fn` that hands out actors from a pool of its own, keeping their
  step, keeps them. The fd is also made blocking before it is handed over:
  one that came through the accept poller, or any accepted socket on BSD
  and macOS, was non-blocking, so the wait for the next keep-alive request
  failed at once and the connection closed after one response.
- **The `--emit-header` file declares the message structs the generated C
  uses (#2517).** It listed a message's fields in declaration order while the
  .c packs ints first, then pointer-sized fields, then the rest, so a C host
  built against the header wrote every field but the first of an interleaved
  message at the wrong offset. Both are now written from one field order.
  The header had also been empty since #996 gated its contents on the
  `--emit=csrc` catalog header, and its typed send helper sent a multi-field
  message with no payload; it now builds the struct and sends it through
  `aether_send_message`, and a single-int message through `payload_int`, as
  the generated code does.
- **The no-networking build links as a shared library on Windows (#2517).**
  The HTTP worker pool and parking lot were built without networking and
  referenced server functions the stubs do not define, and the proxy
  referenced client helpers in the same state. The pool and the lot are now
  built only with networking; the client's clock and header validators,
  which need none, are built always; the two client-bound helpers the proxy
  links against are stubbed.
- **Message structs take their natural alignment (#2509).** Messages with
  more than four fields were declared `aligned(64)`, but their payload is a
  `malloc`'d copy, 16-byte aligned, read through a pointer of that type:
  undefined behaviour, and an aligned vector move would fault. The
  alignment bought nothing for a copy read once and padded every such
  message to 64 bytes.
- **A threadless Windows build compiles again.** `-DAETHER_NO_THREADING` on
  MinGW (`make ci-coop`, `make stdlib EXTRA_CFLAGS=-DAETHER_NO_THREADING`)
  failed in `aether_thread.h`, which named `DWORD` without `<windows.h>`, and
  in the cooperative scheduler's `Sleep` call.
- **An actor still alive when the scheduler's tables are discarded ends as
  a release ends it.** Since 0.790.0 the scheduler frees the actors it
  spawned when it discards a lifecycle's tables; now each also has its state
  destroyed and its queued messages dropped, its block goes back through the
  allocator that made it, an actor its own thread runs is left to that
  thread to end, and the tables are taken under each core's lock, so an
  actor thread exiting meanwhile never reads a freed table. An actor a
  caller passed to `scheduler_register_actor` stays the caller's.
- **`scheduler_wait` returns once an actor thread has handled its messages.**
  A message an actor with its own thread (`auto_process`) handled was
  counted as sent but never as processed, so `scheduler_wait`, and
  `scheduler_shutdown` with it, waited forever in a host using actor
  threads. The thread credits each message it handles, as a core does.
