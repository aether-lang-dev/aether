- **A tuple a call hands back from a statement temporary is copied, and the
  temporary goes with its statement.** `t, u = pair(make_item(w).name,
  make_item(w).tag)`, where `pair` returns its parameters as they came, kept
  its argument structs alive for good, as the string case did before #2619's
  first half: each string position is now copied where the call is made, the
  destructure takes it owned, and the structs are destroyed after the
  statement. A tuple holding a pointer or an array still keeps its argument
  alive (#2619).
