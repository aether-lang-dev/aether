- **A send and a step no longer pay atomic read-modify-writes on counts one
  thread owns (#3541).** A mailbox is written by one thread at a time (the
  core that runs its actor, or the actor's own thread, #3673), so its count
  is now read and stored by that thread instead of atomically added to and
  subtracted from; so are a core's own sent and handled counts, which
  `wait_for_idle()` compares, while the threads that are no core's keep
  atomic counts of their own. A core counts its load (`work_count`) as it
  drains its queues, instead of every sender adding to it on a line of the
  receiving core's, and the loops that step an actor many times yield to
  the queues on one hint word that senders set, instead of loading the
  head and tail of all 17 queues after every step. `make bench-core`
  (best of 5): a one-int send went from 37.8 to 24.4 ns, a two-int one
  from 46.2 to 35.9; the Savina ping-pong from 91 to 63 ns per message and
  the thread ring from 46 to 35.
