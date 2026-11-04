- **A value made for one use and given straight to a builtin, an extern or a
  comparison is released once it has been read (#3549).** `atoi(os.getenv(k))`,
  `char_at(f(), 0)`, `str_eq(f(), s)`, `print("%s", f())`,
  `spawn_sandboxed(g, os.argv0(), a)`, `os.getenv(k) != null`, an ask's reply
  given to `println`, and a buffer `make` handed out for an argument
  (`total(make([]int, 3))`) each leaked the temporary on every evaluation;
  LeakSanitizer found them in the corpus. A parameter borrows, so the caller
  releases a fresh argument after the call: every call lowered by name (a
  builtin, a container store's key, a named function or extern) and every
  comparison now goes through that one rule, which takes over only where the
  callee keeps or ends the value (a free, `isolate`, `box_closure`, a
  container's adopted element, a `@keep` slice).
