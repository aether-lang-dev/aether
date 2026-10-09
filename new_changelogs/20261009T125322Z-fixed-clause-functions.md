- **A function written as several clauses keeps every clause when it is
  imported.** A clause set exported from a module merged only its first
  clause, so `clauses.label(4)` for `label(0)` / `label(n: int)` returned
  "" instead of "n4", and a set returning nothing failed in gcc. Every clause
  of an imported set is merged, through a re-exporting module too, and what a
  later clause calls stays in the build (#2643).
- **Each clause of a clause set is a function of its own.** The clauses are
  emitted as separate functions behind a dispatcher that tries each clause's
  patterns and guard in order, so a clause gets everything a single
  function's body gets: a closure that mutates a clause parameter mutates it
  (it changed a copy), a clause storing into its struct parameter's field no
  longer crashes, a fixed-size array parameter compiles, closures in two
  clauses can capture same-named locals of different types, a guard can call
  a function on its parameter (`when string.length(name) > 3` read an
  undeclared name), and a clause that keeps a `string` parameter takes a
  reference of its own, so its caller frees the temporary it passed. A
  clause set whose first clause has a wildcard pattern, and a builder
  written as clauses, compile (#2644).
- **An unannotated clause set whose first clause returns nothing and a later
  one a value compiles.** Its return type is decided over every clause, the
  same for its prototype and its definition (`int` there, a clause that
  returns nothing giving `0`); the prototype read the first clause alone
  (#2645).
- **A clause set returning a tuple returns a zeroed tuple when no clause
  matches**, with an owned empty string at each string position the caller
  frees; the default was written `0` and gcc refused it (#2646).
- **A clause's `requires` applies only to the calls that can reach that
  clause.** The compile-time precondition check applied the last clause's
  `requires n > 0` to `name_of(0)`, which a first clause without a
  `requires` takes. A clause whose literal pattern or guard the call's
  constant arguments rule out is passed over, and no clause after one the
  call surely matches is checked (#2647).
