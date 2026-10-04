- **`sandbox.enforce` contains its own thread, not the whole process.** The
  sandbox stack the checks walk was one for the process, so while a block
  ran, every other thread (an actor's, a `std.worker`'s) was checked
  against its grants, and the stack was written by threads without a lock.
  It is per thread now: work a block hands to another thread runs with that
  thread's authority.
