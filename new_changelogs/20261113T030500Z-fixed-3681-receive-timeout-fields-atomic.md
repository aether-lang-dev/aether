- **Actors: `wait_for_idle` reads a receive timeout's countdown without a
  data race (#3681).** An actor's `receive ... after` countdown is armed and
  disarmed by its step, on the core that runs it, while `wait_for_idle`
  scans every actor for an armed countdown on the waiting thread; both
  fields were plain integers, and ThreadSanitizer reported the pair in 13
  programs that use `after` (`make test-tsan`, #3542). They are atomic now.
