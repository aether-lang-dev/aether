- **A scheduler core looks only at the actors that have work (#3540).**
  Whenever no message came in through its queues, a core walked its whole
  actor table, loading the mailbox count and `migrate_to` of every actor,
  idle ones included, and reading the clock for each one with a receive
  timeout: a cache miss per idle actor per pass. Each core now keeps a
  ready list, which an actor joins when a delivery or a step leaves its
  mailbox with a message, a move or a work steal asks something of it, or
  another thread tells the core of it (a move onto the core, a restart in
  place, a countdown armed at spawn), and a min-heap of its actors' receive
  timeout deadlines, which it fires once due and parks no longer than. A
  pass costs the actors with work, not the actors. With a ping-pong pair
  beside idle actors (`make bench-core`'s idle rows, best of 5): 100,000
  idle actors took the pair from 38 to 274 ns per message before and 43
  now; 1,000,000 idle actors (the new `BENCH_IDLE_1M=1` row) from 5,855 to
  80. C code that arms an actor's `timeout_ns` and `last_activity_ns` by
  hand, outside the actor's step, tells its core with the new
  `scheduler_timeout_armed(actor)`; the generated spawn does it for
  `receive ... after`.
