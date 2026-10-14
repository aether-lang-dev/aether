- **A clause set compiles in time near linear in its size.** A set of
  2000 clauses took about 4 s to compile and one of 5000 about 79 s:
  the heap-return classification memoised its answer on the first 64
  clauses only, so each later clause classified the set again, with a
  scan of the set per return; and the set a definition is one of, its
  @c_callback clause, its parameter types and, for a set returning a
  tuple, the typing of its calls were each a scan of the set or of the
  program per clause. Each comes from the program index or a memo now:
  2000 clauses compile in about 0.1 s and 8000 in under half a second,
  to the same C (#2897).
