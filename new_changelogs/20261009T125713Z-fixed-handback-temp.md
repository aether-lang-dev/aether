- **A call that hands back a temporary it was given yields a string its
  caller owns.** `t = pass(k, string.concat(w, "!"))`, where `pass` returns
  its `string` parameter as it came, kept the temporary when the call
  returned it, and the local took it as borrowed, so nobody freed it: one
  leak per call, through every consumer (a binding, a return, an argument,
  an interpolation, a struct field, a list). The value is now the temporary
  when that is what came back and a copy of anything else (a literal, a
  local passed beside it), so each consumer adopts or frees it like any
  fresh string. A view of a struct temporary passed on to another call is
  copied once, not twice (#2649).
- **A temporary a hand-back call does not return is freed whatever its
  shape.** The identity guard released it with `string_release`, which skips
  a plain buffer, so `pass_or(0, io.getenv("PATH"))` or a `path.join` result
  leaked whenever the callee returned something else (#2657).
- **Two string functions that call each other are classified alike.** The
  one classified first could end borrowed while the other, classified after
  it, handed over owned strings, which the first then returned as borrowed:
  `ma` returning `mb(...)` and `mb` returning `ma(...)` leaked a string per
  call. A return or binding of a call to a function whose classification is
  still open counts as owned, as a self-recursive return does, and the
  uniform-heap return copies a borrowed one (#2658).
