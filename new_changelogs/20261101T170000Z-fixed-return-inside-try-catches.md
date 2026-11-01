- **A panic while a `return` inside a `try` computes its value is caught.**
  A `return` with no defers to run popped the try frames before it
  computed the value, so `try { return xs[i] } catch e { ... }` with `i`
  out of range, or a returned call that panics, ended the process with
  "panic outside any try/catch". The value is now computed inside the
  frames and they are popped after, for a single value, a tuple and
  `main`'s return alike, as a return with defers already did.
