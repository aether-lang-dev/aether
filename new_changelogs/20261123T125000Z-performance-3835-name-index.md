- **Compiling a large program no longer grows with the square of its size
  in the checker's and the code generator's name lookups (#3835).** Which
  top-level constant, module `var` or enum member a name is, and whether a
  reference is an entry constant or a module var, was a walk of the whole
  program per question. One index answers them, built when the check
  starts and kept current where a declaration is inserted. On a generated
  program of 2000 constants, vars and functions, typecheck went from 387
  to 78 ms and codegen from 237 to 104 ms (the new `names2000_phase_*`
  rows of benchmarks/core).
