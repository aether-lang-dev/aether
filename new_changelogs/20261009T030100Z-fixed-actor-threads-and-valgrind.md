- **An idle actor thread sleeps instead of spinning, and so does its core.**
  An actor with its own thread (`auto_process`) spun on an empty mailbox for
  as long as it lived, a whole CPU each, and the core it was on counted it as
  work on every pass, so that core never went idle either. The thread now
  spins a while, as a core does, then parks until a message, its release,
  the scheduler's stop or its teardown wakes it; asleep, it no longer holds
  back the freeing of released actors. The core skips it (#2592).
- **An actor with its own thread no longer loses messages or runs on two
  threads.** Its messages went through a single-producer queue that the
  core, the core's other actor threads and the actor itself all wrote: two
  sends at once could write the same slot, a full queue (63) dropped the
  message, and the thread moved what it found into a 32-slot mailbox and
  dropped the rest. Each dropped message had been counted as sent, so
  `scheduler_wait()` then waited forever. Every send to such an actor now
  goes into an inbox of its own that takes any number of senders, never
  drops a message and keeps each sender's order, and whatever is left in it
  when the actor ends is released. A same-core send could also step the
  actor inline on a second thread, as could a send in main-thread mode, and
  work stealing could move it; none does now (#2598).
- **The Valgrind CI job fails on memcheck errors.** It looked for invalid
  reads, writes and frees by name and threw the run's exit status away, so
  main passed with 86 uninitialised reads: `create_code_generator` never
  set `lib_actors`, which every codegen unit test then read. The generator
  is zeroed at creation, and the job now fails on any memcheck error, any
  failed test and any leak. It also runs Valgrind with `--fair-sched=yes`,
  writes Valgrind's report to a file of its own, and lists the slowest tests
  in its summary (#2599, #2593).
- **`std.udp` fails at once on a host with a space or a control character.**
  No address or name can contain one, and on macOS the system resolver
  waited out a DNS timeout, about 5 s, before saying so (#2596).
