- **Leaving a trailing block early no longer leaks its builder context.**
  `f() { ... }` pushes a builder context before the block and pops it after,
  but the pop only ran when the block fell off its end: a `return` from
  inside the block (a `bldr.build() { ... return 1 }` error exit), or a
  `break`/`continue` out of a block inside a loop, skipped it. Each such exit
  leaked one context; once the 64-deep stack filled, pushes were silently
  dropped, so later blocks' DSL calls attached to a stale parent and their
  pops tore down the caller's real contexts. The pop is now a defer of the
  block's own scope, so every exit runs it, after the block's own `defer`s.
  An unlabeled `break`/`continue` also now runs the defers of every scope it
  leaves inside the loop body, as a labeled one already did, not only the
  innermost scope's.
