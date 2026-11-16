- **A core's ready lists and timer heap are made at `scheduler_init` (#3540).**
  They were allocated on a core's first push or first receive timeout, so a
  program's steady work allocated them whenever it first reached a core,
  memory held for the lifecycle. A test that measures a window of steady
  work for growth (test_actor_ref_state_store) read that as a leak of 512
  bytes in 4 of 20 runs; it passes 20 of 20 now. Each core makes room for
  64 entries in each at init and grows past that on demand.
