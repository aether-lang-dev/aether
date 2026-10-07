- **Running an `--emit=lib` program's `aether_main()` again no longer leaks the
  previous run's actors.** Nothing frees an actor (an executable just exits),
  so when a second `aether_main()` re-initialized the scheduler and discarded
  the previous run's per-core actor tables, every actor still registered in
  them became unreachable. macOS `leaks` caught it only intermittently -- a
  stale pointer elsewhere often kept the actor "reachable" -- so
  tests/integration/emit_lib_keeps_main's "leaks after a rerun" check failed
  on some CI runs, `main` included. The scheduler now frees the actors
  registered in a table when it discards that table, after the scheduler
  threads have been joined; an actor is only ever in one table (migration and
  stealing move it), so none is freed twice.
