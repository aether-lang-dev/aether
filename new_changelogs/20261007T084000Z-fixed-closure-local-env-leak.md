- **A capturing closure bound to a local frees its environment when the scope
  ends (#2480).** `g = || { println(n) }` allocated `g`'s environment and
  never freed it, so every call of the enclosing function leaked it, and
  with it every shared cell the closure writes and the string or struct
  strings that cell holds. The scope-exit free was only queued for a name
  the closure registry had not seen yet, and the registry records every
  closure binding before any code is emitted, so it was never queued. Now
  a local bound only to closure literals frees its environment once at
  scope exit (and on `return`, `break` and `continue`), through the
  closure's destructor, which also releases its cells and strings; a
  closure rebound in a loop frees the one it replaces. The free is skipped
  whenever the value can outlive the scope: returned, aliased, stored into
  a struct, list, map, message, global or actor state, passed to a
  parameter that keeps it (or to a function whose body cannot be seen), or
  captured by a closure that does any of these.
