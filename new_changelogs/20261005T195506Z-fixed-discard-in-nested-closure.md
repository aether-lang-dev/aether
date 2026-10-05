- **`_ = f()` inside a closure nested in another closure compiles again.**
  Each `_ = ...` (and each `_` slot of `a, _ = g()`) reads like a declaration
  of `_`, so when a closure or trailing block that discards sat inside another
  that also discards, capture analysis took the inner `_ = ...` for a write
  through to the outer `_` and captured it. The generated C then built the
  inner closure's env from an undeclared `_` and failed to compile. `_` names
  no storage and is now never a closure capture, at any depth; real captures
  are unchanged.
