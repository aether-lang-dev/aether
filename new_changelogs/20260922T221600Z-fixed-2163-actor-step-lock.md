- **An actor's `step_lock` is released only by whoever took it (#2163).**
  `aether_step_safe` cleared it on its panic path, but it never acquired it
  — all seven call sites take it before calling and clear it after. So a
  panicking actor handed the lock away while its caller still believed it
  held it: another thread could acquire it and start stepping the same
  actor, the caller then cleared a lock it no longer owned, and a third
  could enter. Two threads inside one actor's step corrupt the heap, which
  is how this surfaced — `free(): invalid pointer` from the #2083
  regression test on CI. Four of the call sites step in a loop under a
  single acquisition, so the lock was being dropped mid-loop.
  `tests/integration/actor_panic_step_lock` pins the ownership rule by
  reading the source (a race cannot be disproved by running a program, but
  a rule about who owns a lock can be checked by reading) and exercises the
  path with an actor that panics while two worker threads and the main
  thread send to it.
