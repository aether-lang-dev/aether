- **A string argument held for a call skips the journal work that does
  nothing (#3840).** A string read from a field, an element or a module
  `var` and passed to a call is held for the call, and was journaled for a
  panic and forgotten after it with two out-of-line calls even outside
  every `try`. `aether_unwind_track_place` now says whether it wrote the
  entry, as `aether_unwind_track` does since #3787, and the forget runs
  only then; a closure argument does the same. A panic still releases
  every hold that was journaled. The runtime ABI revision is 8. A held
  call went from 1.90 to 1.51 ns (the new `string_held_arg` row).
