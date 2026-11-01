- **An `if` used as a value parses when its condition ends in a call
  (#3478).** `s = if leap(year) { "leap" } else { "common" }` failed with
  "Expected LEFT_BRACE, got ELSE": the expression form parsed its condition
  without the guard the `if` statement uses, so the call took the then
  arm's `{` as a trailing block. A call inside an arm still takes one.
