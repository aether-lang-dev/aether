- **A panic while a `return` inside a `try` computes its value is caught,
  and `main` computes what it returns before its defers run.** A `return`
  with no defers to run popped the try frames before it computed the
  value, so `try { return xs[i] } catch e { ... }` with `i` out of range,
  or a returned call that panics, ended the process with "panic outside
  any try/catch". The value is now computed inside the frames and they are
  popped after, for a single value and a tuple, as a return with defers
  already did. `main` ran its defers, and released its owned locals,
  before computing the value it returns: `return string.length(s)` read a
  released `s`. It now computes the value first, as every other function
  does.
