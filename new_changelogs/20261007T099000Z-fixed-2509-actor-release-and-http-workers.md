- **Releasing an actor is safe while the scheduler reads its table (#2509).**
  `scheduler_release_actor` freed the actor without taking it out of its
  core's table, so the idle scan, `aether_scheduler_poll` and
  `scheduler_wait` went on reading freed memory. Since #2486 those readers
  take no lock and may hold a pointer loaded before a removal, or walk a
  table grown out of that still holds a stale copy. A release now marks the
  actor dead, takes it out of its table under the core's lock and retires
  it. Every core's scheduler thread publishes the epoch it has seen at the
  top of its loop, any other thread that walks the tables holds an epoch for
  the walk, and a retired actor is freed once every published epoch is past
  the one it was retired in. The read path takes no lock. A release from the
  actor's own step, including one an inline (main-thread-mode) send is
  running, waits until whatever ran the step is done with it, in the
  cooperative scheduler as well.
- **`scheduler_deregister_actor` finds an actor that is migrating (#2509).**
  It read `assigned_core` before taking that core's lock, so an actor moved
  to another core in between was not removed. The core is now read again
  under the lock, and the lookup retries until it names the core held.
- **Main-thread mode is entered once per scheduler lifecycle.** Released
  actors bring the actor count back to zero, and the next spawn would have
  put that actor in main-thread mode, stepped inline on whichever thread
  spawned it, with the scheduler threads already running. Releasing the
  lone actor of main-thread mode now leaves the mode, which had kept
  pointing at the freed actor.
- **An HTTP server in actor dispatch mode releases its workers (#2509).** It
  spawned a worker actor per connection and never called the `release_fn`
  it was given, so each request left an actor allocated and registered.
  The worker now runs the server's own step, which runs the step it was
  given on the connection message and then releases the worker. The fd is
  also made blocking before it is handed over: one that came through the
  accept poller, or any accepted socket on BSD and macOS, was non-blocking,
  so the wait for the next keep-alive request failed at once and the
  connection closed after one response.
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
