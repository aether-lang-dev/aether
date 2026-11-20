- **An actor's mailbox ring is 16 slots, not 32, and the size can be chosen
  at build time.** The ring was 1552 of an ActorBase's 1720 bytes. At 16 an
  actor is 43% smaller (a two-field actor is 1024 bytes where it was 1792),
  and 1M idle actors peak at 721 MB where they took 1816; the send, ask and
  idle rows of `make bench-core`, a new fan-in row (eight actors flooding
  one) and skynet did not move. A burst past the ring waits in the actor's
  overflow inbox, as before, so nothing is lost. `-DMAILBOX_SIZE=n` (a power
  of two) chooses another size; the runtime and every program and library
  built against it must use the same one, and another size is part of the
  runtime ABI id (`+mailbox32`), so a library built with one is refused by a
  runtime built with another. The runtime ABI revision is 4. The size of
  ActorBase is checked against the notes that give it (#3776).
