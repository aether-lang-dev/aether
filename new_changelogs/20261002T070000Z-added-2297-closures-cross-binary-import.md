- **Closures cross a binary import.** An exported function that takes or
  returns a closure (bare `fn`) was left out of an `--emit=lib` library
  with a warning. Now the closure crosses by value as `_AeClosure`, which
  carries its function and its captured environment. A library calls the
  closure a program passes, and a program calls one the library returns,
  captures included. The catalog's source signature spells the type `fn`,
  so the interface `ae` builds for an importer declares it. Only an Aether
  importer knows the `_AeClosure` layout and calling convention, so an
  `--emit=csrc` header leaves these prototypes out, as it does for a struct
  passed by value (#2297).
