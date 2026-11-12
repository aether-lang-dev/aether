- **A send and a step no longer pay atomic read-modify-writes on counts one
  thread owns (#3541).** A core's sent and handled counts, which
  `wait_for_idle()` compares, are written by its own thread alone and now
  added to with a load and a store instead of an atomic add at every send
  and every inline step; the threads that are no core's keep atomic counts
  of their own. A core counts its load (`work_count`) as it drains its
  queues, instead of every sender adding to it on a line of the receiving
  core's, and the loops that step an actor many times yield to the queues
  on one hint word that senders set, instead of loading the head and tail
  of all 17 queues after every step. `make bench-core` (best of 5): a
  one-int send went from 38.6 to 33.6 ns, a two-int one from 45.9 to 42.3;
  the Savina ping-pong from 92 to 82 ns per message and the thread ring from
  47 to 44. The mailbox count keeps its atomic add and subtract: a caller's
  actor registered again after a restart can still have its mailbox written
  by one core while another steps it.
