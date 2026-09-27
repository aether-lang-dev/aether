- **A local holding a function pointer now shadows a same-named builtin
  (#2211).** `release = h.release` followed by `release(state)` was
  type-checked through the local but lowered as the `release` builtin,
  because codegen's by-name builtin dispatch ran before its typed
  fn-pointer local branch: a type error went to stderr, the build still
  exited 0, and the binary never called the hook. A local named `free`
  had the same defect plus a spelling mismatch (`ae_free` at the call,
  `free` at the declaration) that broke the C compile. The fn-pointer
  branch now runs first and spells the local as its declaration does,
  and a genuine `release()` on a non-string is a reported error, so the
  build fails instead of emitting a binary beside an error line.
  `tests/regression/test_local_fnptr_shadows_builtin.ae` and three
  `test_codegen.c` cases cover it.
