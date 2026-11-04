- **Merging a large module and compiling many extern calls no longer grows
  with the square of their size (#3546).** The module merge walked the
  program and the module for every declaration it merged and every
  reference it renamed, and the code generator walked the extern table for
  every extern call; both are now one probe of an index. Merging a module of
  8000 functions and 8000 vars took 7.5 s of CPU and takes 94 ms, and
  writing 8000 extern calls 1.5 s and 63 ms (aetherc --time-phases, best of
  5); a std-heavy program (benchmarks/core/compile_heavy.ae) compiles in
  63 ms of CPU where it took 78.
