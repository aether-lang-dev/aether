- **`rows = append(rows, f())` on a slice a closure shares no longer
  corrupts the heap (#3568).** The append emptied a temporary copy of the
  shared slice instead of the slice itself, which then gave back the same
  reference again. It empties the slice itself now, after the value is
  read.
