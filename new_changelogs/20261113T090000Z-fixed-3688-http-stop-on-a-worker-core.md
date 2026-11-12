- **std.net: a stop called from an actor no longer hangs an actor-dispatch
  server (#3688).** The run waited for every worker actor given a
  connection, and a worker still queued can only be stepped by its own core:
  a stop called in an actor's handler on that core (or on a one-core
  scheduler) blocked the core until the run ended, and the run waited for the
  worker, so the process hung. The run now waits only for the workers
  serving a connection, which finish the request they are in, as before. A
  worker still queued holds a reference to the server: when it is stepped it
  closes its connection without serving it, and the server is freed by
  whoever gives back the last reference, the program's `http_server_free` or
  that worker. `aether_http_server.h` states the contract on
  `http_server_stop` and `http_server_free`. Covered by
  `tests/integration/http_actor_mode_release`, which stops the server from an
  actor on a queued worker's core, and again on one core where `taskset` is
  available.
