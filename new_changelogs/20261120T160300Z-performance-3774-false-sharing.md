- **Hot words other threads write no longer share a cache line with words
  a loop reads.** The scheduler core's `running`, which its loop reads on
  every iteration, sat on the line of the lock every non-core sender takes
  per send; a spawn's actor id counter beside the reclamation epoch every
  core loads per iteration; the HTTP event loop's `stopping`, polled by
  every driver, beside the counters each connection's start and end write;
  and the memory cap's count, written by every allocation under a cap,
  beside the words every allocation and every loop back-edge read. Each
  group has a line of its own now, the padded spinlock is aligned to the
  line it pads, the event loop and its drivers are allocated at that
  alignment, `_Static_assert`s keep the pairs apart, and arming a deadline
  stores its flag only when it is not set yet (#3774).
