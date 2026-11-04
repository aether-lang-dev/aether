- **A slice local given in an `if` to a borrowing parameter stays
  usable (#3568).** `total(if c { xs } else { mk(2) })` moved `xs` into the
  call's temporary although the parameter only borrows it, so `xs.len` read
  0 after the call. A branch that is a place gives the call a copy now, and
  one given to a `@keep` parameter moves there by the move checks too, as a
  plain argument does.
