- **A closure's write reaches a local first bound in a branch or a loop.**
  A local first bound in an `if`'s branches (or a switch's cases, or a
  while body) and then rebound by a closure kept its old value: the closure
  declared a C local of its own, though the local is one variable for the
  function. `if k > 0 { n = 1 } else { n = 2 }` then `rebind = || { n = 7 }`
  and `rebind()` left `n` at 1, and the same for strings and arrays. Such
  a closure now writes the variable (#2901).
