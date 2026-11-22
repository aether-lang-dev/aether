- **A parameter that a closure in a trailing block captures is the
  parameter, not the block receiver's function of the same name.**
  aether-ui's `reorderable(cb: fn)`, whose block passed a closure calling
  `cb` to a DSL that also had a `cb` setter, read the setter where it meant
  its parameter, and the C compiler stopped at an int-to-pointer conversion;
  sae's gate check kept every such name apart because of it. The same root
  as the fix above (the receiver was asked before the program's own
  bindings); this adds its regression test, with the parameter both
  captured by a closure and passed on as a value.
