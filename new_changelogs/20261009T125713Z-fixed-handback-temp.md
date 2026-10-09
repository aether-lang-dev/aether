- **A call that hands back a temporary it was given yields a string its
  caller owns.** `t = pass(k, string.concat(w, "!"))`, where `pass` returns
  its `string` parameter as it came, kept the temporary when the call
  returned it, and the local took it as borrowed, so nobody freed it: one
  leak per call, through every consumer (a binding, a return, an argument,
  an interpolation, a struct field, a list). The value is now the temporary
  when that is what came back and a copy of anything else (a literal, a
  local passed beside it), so each consumer adopts or frees it like any
  fresh string. A temporary the call does not hand back is freed whatever
  its shape (a plain buffer such as `path.join`'s was skipped), a view of a
  struct temporary passed on to another call is copied once, not twice, and
  two string functions that call each other are classified alike, so one no
  longer hands the other's owned result over as borrowed (#2649).
