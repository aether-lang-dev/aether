- **Actors: an actor's own thread no longer stays behind once it ends (#3689).** An
  `auto_process` actor runs on a thread of its own, which nothing ever
  joined: when it ended, the C library kept its stack and its record until
  the process exited, one per such actor that ever ran. ThreadSanitizer
  reported the leaked thread (`make test-tsan`, #3542). The thread is
  detached when it starts, so it gives everything back as it ends.
