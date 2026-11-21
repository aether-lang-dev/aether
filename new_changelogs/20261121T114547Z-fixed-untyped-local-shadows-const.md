- **A local whose value the compiler types late still shadows a constant of
  its name.** With `const f = "text"` in the program, a function binding
  `f = list.get(xs, 0)` (or any value the early type-inference pass cannot
  type on its first walk: a call whose result is inferred from its body, a
  field read through a pointer, a destructured slot of such a call, an if-
  or match-expression over one) and then reading `g = f` failed with E0200
  "Type mismatch in variable initialization": the walk bound the local only
  once it had a type, so the read took the constant's. A local named like a
  struct, a message, an actor or a selectively imported constant failed the
  same way (E0302 "Unknown type" for a struct or an actor). The walk now
  binds the name as soon as it is bound, typed or not, except a bare binding
  of a module-level `var`, which writes the global as it always did.
