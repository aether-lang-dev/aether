- **A fixed-size array as a result is refused with E0201, not left to fail
  in C.** No function can return a `T[N]`: C returns no array, and the
  code generator does not wrap one, so `pair() -> int[2]` compiled to
  `int[2] pair(void);` and a struct literal given such a call
  (`Row { docs: two("w") }`) failed in `_aether_dup_each_Doc`. The type
  checker now refuses the result, alone or at a position of a tuple,
  written or inferred, of a function, a closure or an extern, and a
  function type with such a result written for a parameter, a field or a
  local, with a help line naming the struct to return instead
  (`struct Pair { v: int[2] }`). No program that built before is refused.
  Returning the array itself is #2894. Closes #2886.
