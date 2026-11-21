- **A function's write to a module `var` keeps the var's type for the reads
  after it.** With `var f: float = 0.5`, a function doing `f = 2` and then
  `g = f` declared `g` an int, so C cut the 2.0 the global held to 2 and
  `g / 4` was 0 where the answer is 0.5, with no diagnostic; a destructured
  slot (`f, s = pair()`) did the same. The early type-inference pass took
  the binding for a local of the value's type. It is the global, of the
  global's type, so the reads are floats. A module's code still never
  reaches the program's own vars: there the binding is the function's
  local, as before.
