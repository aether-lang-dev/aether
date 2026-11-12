- **`scheduler_send_local` from a thread that is not the actor's core no
  longer writes its mailbox (#3672).** From a thread no core owns (a C host's main
  thread, say) it put the message straight into the actor's mailbox while
  the actor's core thread delivered into the same mailbox: two producers on
  a ring that takes one, so messages overwrote each other or were taken
  twice. Such a send now goes through the actor's core, as
  `scheduler_send_remote` sends it. In the new runtime test, the main
  thread and an actor of the target's core each send 100,000 messages at
  once; before, 12,990 arrived out of their sender's order (overwritten or
  repeated), and now every one arrives once, in order.
