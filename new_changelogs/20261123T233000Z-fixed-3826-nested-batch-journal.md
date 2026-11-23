- **A nested batch no longer drops the outer batch from the unwind
  journal.** A batch opened inside an open one, on the same thread, updated
  the one journal entry the thread's batch had and forgot it at its end, so
  a panic in the outer loop after the inner one closed left the outer
  batch's messages in the buffer until the thread's next send or wait; an
  inner batch inside a `try` of its own moved the entry to that `try`,
  whose unwind took it. The runtime now journals one entry per `try` depth
  with an open batch, the first batch opened there journaling it and the
  last one closed forgetting it, so the outer batch stays journaled until
  its own end (#3826).
