- **A deregister from a host thread waits for a core still looking at the
  actor, so the free after it is safe (#3718).** Since v0.919.0
  `scheduler_deregister_actor` only took the actor out of its core's table,
  but a core that had just found it there went on to read and write it (a
  ready list entry's look, a due receive timeout's step, a timer heap move),
  so a free right after the call could land under that use. Each core now
  names the actor it looks at, and a deregister on a thread that runs no
  actors waits for that look to end: at most the step under way, never a
  countdown, never on its own core. From inside a step it does not wait for
  a step of the actor on another core (two steps deregistering each other's
  actors would wait on each other); the header says when that caller may
  free.
