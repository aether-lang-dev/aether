- **`ae fmt` writes the type prefixes tight.** It put a space after every
  `]` followed by a word, so `make([]int, n)` became `make([] int, n)` and
  an enum-indexed array `[Dir]string` became `[Dir] string`, unlike the
  language reference. A `]` closing a prefix group (empty, or holding one
  name) before a type name or `*` is now written tight; an index before a
  word (`a[0] as long`) keeps its space (#2316).
