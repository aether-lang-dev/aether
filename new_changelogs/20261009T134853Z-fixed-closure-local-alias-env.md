- **A closure local bound to another holds a reference of its own.**
  `get_last = get`, with `get` made in a loop body and `get_last` declared
  before the loop, leaked an environment per pass: `get` stopped owning its
  value at the copy and `get_last` never started. The copy now retains, so
  each local releases its own at its scope exit or its next binding; a swap
  through a third local, an alias kept in a list, and an alias made inside
  a closure of a closure it captured are released alike (#2668).
- **A closure local named like a struct's `fn` field is released.** A
  local `set` beside `Hooks { set: ... }` or `h.set` in the same function
  had the field's name taken for a use of itself, which kept its
  environment for good: every closure stored from it leaked one (#2669).
