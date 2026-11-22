- **contrib.quickjs: a budget of work, and the exception as a handle.**
  `quickjs.set_poll_budget(q, polls)` stops JavaScript after so many of the
  engine's interrupt polls rather than so much time (quickjs-ng polls once
  every 10,000 calls, jumps and built-in iteration steps, and a regexp on its
  own counter), the same on any machine: `INTERRUPTED_BUDGET` (4) says so,
  `poll_budget` reads what is left and `clear_poll_budget` removes it. A
  budget of n stops a fresh runtime after exactly (n - 1) * 10000 steps.
  `quickjs.last_exception(q)` is the value the last failure threw, as a new
  handle the caller releases, so `message`, `name` and `stack` are read apart
  instead of cut out of `last_error`'s text (a message can hold a newline);
  0 when the failure had no value. The README's claim that a long regexp is
  not interrupted was wrong: the regexp engine polls too, and a runaway one
  is stopped.
