- **A closure passed to another module's function sees its sealed
  function's locals.** Under `seal except`, `mm.run_it(|| { ... ctx ... })`
  failed with E0300 `Undefined variable 'ctx'` for a local `ctx` of the
  sealed function, while the same closure passed to a function of the same
  file compiled. The closure of a member call also resolves names through
  the module as a DSL receiver, and the seal ended that whole lookup instead
  of only the search for the module's member.
