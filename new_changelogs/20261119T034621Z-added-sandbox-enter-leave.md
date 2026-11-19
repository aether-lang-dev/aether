- **`sandbox.enter(g)` / `sandbox.leave()`: enforce's two halves,
  documented and exported.** A level that is not one block (a host's entry
  helper that puts a guest's grants in force and takes them off later) was
  reachable only through the undocumented `sandbox_push` / `sandbox_pop`
  builtins. `enter` lends the list (no copy, unlike `enforce`: a grant added
  later applies, and the list must outlive the `leave`), nests by
  intersection with `enforce` blocks and other `enter`s, counts toward the
  64 levels a thread holds (the 65th panics), and `leave` with nothing in
  force is a no-op; `leave` takes the innermost level whoever pushed it, so
  the caller pairs them (`defer sandbox.leave()`).
  `std/sandbox/test_sandbox_enter.ae` checks each of those.
