- **A panicking drop of a message still on its way to a killed actor no
  longer ends the program.** A message sent before the kill but not yet
  delivered is dropped by the core that later finds its target dead, and that
  drop ran its fields' drop functions with no frame around them: a drop that
  panicked aborted the process ("panic outside any try/catch or actor"). It
  now runs contained and is reported through the death hook, as the
  finalize's own drain of the mailbox already was (#3401). This was the
  intermittent `emit_lib_actor_step_context` failure on CI (about 1 run in 100
  under load, most often on macOS x86_64), which could block a release.
  `tests/regression/test_actor_killed_in_flight_drop_panic.ae` aborts in
  nearly every run without the fix.
