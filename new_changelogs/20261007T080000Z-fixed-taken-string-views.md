- **A string taken from a struct field, an `if` or a `match` is owned by
  what it is stored into (#2461).** `t = r.name` stored the field's pointer
  without owning it, so reassigning `r.name`, replacing `r` or leaving `r`'s
  scope freed what `t` still pointed at; the same held for a struct literal
  field, a field store, a returned value, a module global, actor state and a
  closure's string cell. An `if` or `match` whose arm was a local string did
  the same once the local was reassigned or its function returned, and a
  `match` binding never updated its local's ownership at all, so the value it
  replaced leaked and a later free could hit a literal. Each of these now
  takes the value the way `b = a` already did: a field read is copied, a
  local is moved on its last use and copied otherwise, a freshly built string
  is adopted and a literal is borrowed, so every buffer is freed once. A
  function that returns a field read now returns a copy its caller owns.
