- **A send whose target panics no longer leaks its message.** A send took
  its message's fields (a string copied, a received string moved on, #3827,
  the rest published) and then read its target, so a target that panicked,
  an index past a fixed array's end or a call that panics, left them to no
  one. The target is now read first, in source order, before any field is
  taken: its panic leaves the received message whole, and the handler's
  release gives everything back. A null target is still released before
  its panic (#3847).
