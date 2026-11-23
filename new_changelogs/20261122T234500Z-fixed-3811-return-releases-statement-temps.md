- **A `return` gives back every temporary its statement made (#3811).** A
  return chose its C shape before emitting its value, from a guess at
  whether the value would hold a temporary for its statement, and a value
  the guess missed was returned bare, its temporary's release emitted after
  the `return` where it never ran. `return gs.len` of a module-level slice
  `var` kept one reference to the var's buffer per call, so a buffer the var
  let go of later was never freed, and inside a `try` left a journal entry
  naming a slot of a frame that had returned. Every return with a value now
  computes it into the result, then gives back the statement's temporaries
  and runs the scope's defers, then returns; the guess is gone.
