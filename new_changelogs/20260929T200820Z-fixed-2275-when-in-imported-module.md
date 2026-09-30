- **A top-level `when` in an imported module keeps its surviving arm
  (#2275).** A module's top-level `when` was left for the merge, which
  copies only declarations, so the selected arm's externs, functions and
  constants never reached the program, and the module's own functions
  reported them undefined. The same `when` in the entry file worked. A
  module's `when`s are now resolved as soon as it is parsed, so the
  surviving arm is the module's top level for its imports, its exports and
  the merge. A `when` condition that is not a compile-time constant is
  reported against the file it is in and fails the build.
