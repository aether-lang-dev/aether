- **The plain C path has no GNU statement expression left in the sweep.**
  Four `string?` constructs (a `string?` parameter a closure writes, a
  destructure into a `string?` cell or module var, a `??` over a `string?`
  local returned) were still spelled as statement expressions whatever the
  C compiler, so cl.exe and a strict C11 compiler rejected the programs
  using them. A cell or global now binds the value and then takes its
  string as a statement, and the returned `??` is a sequence of
  temporaries declared ahead of the return, as the rest of that path is.
  CI's strict C11 step now checks the C of every program the sweep builds,
  reading the C the sweep kept (`AE_SWEEP_KEEP_C=1`, ae's `AE_KEEP_C`), so
  a later construct cannot bring one back unseen (#2885).
