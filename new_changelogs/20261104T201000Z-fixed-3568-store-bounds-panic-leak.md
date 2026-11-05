- **A slice stored past the end of a slice is no longer leaked by the panic
  (#3568).** `rows[k] = mk(2)` on a slice of slices too short for `k` made
  the new buffer before the bounds check, which then panicked with the
  buffer held by nothing: a caught panic leaked it each time, and so did a
  store into a slice field of an element (`hs[k].tags = mk(3)`). The place
  is checked before the value is made now, as a string, a closure or a
  struct element's store already is.
