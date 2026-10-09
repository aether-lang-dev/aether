- **An idle actor thread sleeps instead of spinning.** An actor with its own
  thread (`auto_process`) spun on an empty mailbox for as long as it lived,
  a whole CPU each. It now spins a while, as a core does, then parks until a
  message, its release or the scheduler's stop wakes it; while asleep it no
  longer holds back the freeing of released actors (#2592).
- **An actor with its own thread no longer loses messages.** Its thread
  moved up to 128 queued messages into a 32-slot mailbox and dropped the
  rest, and a send from its core dropped the 64th queued message; both had
  been counted as sent, so `scheduler_wait()` then waited forever. A send
  from a core-mate could also step it inline on a second thread while its
  own thread stepped it, and work stealing could move it. All four are
  fixed (#2598).
- **The Valgrind CI job fails on memcheck errors.** It looked for invalid
  reads, writes and frees by name and threw the run's exit status away, so
  main passed with 86 uninitialised reads: `create_code_generator` never
  set `lib_actors`, which every codegen unit test then read. The generator
  is zeroed at creation, and the job now fails on any memcheck error, any
  failed test and any leak. It runs Valgrind with `--fair-sched=yes`, which
  ends the starvation of threads waiting on a busy one that made two
  scheduler tests take 23 and 40 minutes, and lists the slowest tests in
  its summary (#2599, #2593).
- **`std.udp` rejects a host that cannot exist without asking DNS.** A host
  with a byte no address or name can contain (a space, say) fails at once;
  on macOS the system resolver waited out a DNS timeout, about 5 s, first
  (#2596).
