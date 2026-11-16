- **The spawn's `AETHER_SINGLE_CORE` cache is atomic (#3712).** The
  generated `spawn_<Actor>` kept the variable's value in a plain static,
  and two threads making their first spawn of the same actor type both
  wrote it: a data race. It is now loaded and stored relaxed.
