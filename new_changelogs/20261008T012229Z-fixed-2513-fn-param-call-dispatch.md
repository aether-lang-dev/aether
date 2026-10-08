- **A call on an `fn` parameter goes through the parameter's value (#2513).**
  `f()` on the `fn` parameter of one function used to be bound by name to
  the closure variable `f` of an unrelated function and ran that closure's
  code with the parameter's env, an access violation. The closure a
  variable holds is now recorded per scope, so a parameter, or a name bound
  in another function, dispatches through `f.fn(f.env)`.
