- **A failed bounds check's helpers are out of line, cold and noreturn.**
  The checks stay inline, and their failure is one call laid out after the
  hot path: a function with a checked index has 144 bytes of hot code at -O2
  where it had 256, the rest in `.text.unlikely`; -O0 and -Og builds are
  unchanged. The runtime's rare paths (a send to an actor that has ended, a
  send while the scheduler is stopped, a message dropped, a queue's
  overflow) are cold too: 1408 bytes leave the scheduler's hot code and 336
  the send path's (#3777).
