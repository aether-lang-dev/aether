- **An `fn[N]` local in a loop body releases its closures.** The loop's
  hoist declares the array before the loop, so the release its binding
  pushes was never pushed, and each iteration stored its closures over the
  last one's: every iteration leaked them, more where they captured a
  promoted cell. The hoist now claims the array as it claims a closure
  local: each rebinding releases the closures it replaces (after a
  `continue` too), and the last ones are released where the loop's scope
  ends, which a `break` or a `return` out of the body reaches as well. As
  for a closure local, an element passed to a function that keeps a
  reference of its own, or returned (the return retains it), no longer
  keeps the array from releasing its closures (#2876).
