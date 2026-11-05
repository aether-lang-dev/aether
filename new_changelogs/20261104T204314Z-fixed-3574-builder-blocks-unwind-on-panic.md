- **A panic inside builder blocks no longer leaves them open (#3574).** Only
  a `try` of the program put the stack of builder blocks back after a
  panic. When an actor's handler panicked inside blocks without one, the
  actor's step caught the panic and the blocks stayed on its thread's
  stack: the next handler on that thread found its own block nested under
  them, as `builder_depth()` showed, and their configs were never
  released. Each block now records its depth in the same unwind journal a
  `defer` uses, so the stack goes back to where the panic is caught, and
  the abandoned blocks' configs are released, whatever catches it.
