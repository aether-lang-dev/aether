- **Stopping the scheduler no longer rewrites the queue that threads it does
  not own send through (#3521).** To wake the cores, `scheduler_stop` stored
  each core's foreign queue tail back into itself, for an MWAIT on that word
  that no core makes, and without the lock every other writer of the queue
  takes. A send from an HTTP pool worker or a host thread landing between
  the load and the store had the tail moved back under it: the message was
  lost and its payload leaked, or stale slots were delivered again. The
  cores wake on their own `running` flag and the park broadcast, so the
  store is gone.
