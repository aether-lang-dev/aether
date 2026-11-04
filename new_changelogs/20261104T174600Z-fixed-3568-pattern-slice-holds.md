- **A slice a `match` arm's pattern binds holds a reference of its own
  (#3568).** A list pattern's tail (`[h | t]`), a slice a list pattern binds
  out of a list of slices and a struct pattern's slice field viewed what the
  match was given and held nothing: an arm that bound the subject again read
  freed memory, and one that bound its tail again leaked a buffer each
  time. Each takes a reference where the arm's body starts and gives it
  back on the body's every exit, so `free(t)` of a match tail gives that
  reference back where it was a named panic (a clause's tail, a parameter's,
  still is). A struct pattern's slice field (`Box { xs: s }`) was not bound
  at all, and the generated C named an undeclared `s`.
