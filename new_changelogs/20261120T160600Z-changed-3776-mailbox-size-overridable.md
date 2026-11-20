- **The mailbox ring's size can be chosen at build time.** `MAILBOX_SIZE`
  (32 slots, 1552 of an ActorBase's 1720 bytes) takes `-DMAILBOX_SIZE=n`, a
  power of two, as the scheduler's sizes do; a burst past the ring waits in
  the actor's overflow inbox, so a smaller ring loses nothing. The runtime
  and every program and library built against it must use the same value:
  another value is part of the runtime ABI id (`+mailbox16`), so a library
  built with one is refused by a runtime built with another. The size of
  ActorBase is checked against the notes that give it (#3776).
