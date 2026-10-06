- **Two returns in a row no longer break the C build.** A `return` in a
  function with defers to unwind (an explicit `defer`, or a heap-string local
  the function frees on exit) declared its `_builder_ret` temporary straight
  into the enclosing C block, so a second return in the same block -- a
  `return os.system(...)` followed by an unreachable `return 0` in a
  `bldr.build() { ... }` build file -- failed with "redefinition of
  '_builder_ret'". Each such return now gets its own C scope, for single and
  multi-value returns alike. The unreachable-code warning (W1002) now also
  looks inside trailing blocks, closures, `for` and `match` bodies and bare
  blocks, where it was previously silent, so the redundant return is still
  reported.
