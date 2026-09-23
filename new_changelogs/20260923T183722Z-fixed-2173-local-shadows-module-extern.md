- **A program's local variable no longer retypes a module's extern of the
  same name (#2173).** Type inference walks a program with one symbol table:
  each function's locals are pushed for its walk and popped after it. A
  local named like something already in the table retyped that entry in
  place instead of shadowing it, and popping cannot undo an overwrite. So
  `floor = loader.plane(...)` in one of ae3d's test functions left the
  `extern floor(x: float) -> float` of its `cloudnoise` module typed as a
  pointer. The module's own `floor(x) as int` then failed with "cannot cast
  ptr to int with `as`", in a file the author had not touched, from the
  moment a change elsewhere made the module reachable. An `int` local of
  that name compiled, and silently typed the extern `int`.

  Locals and tuple-destructure targets now follow the rule #1967 set for
  parameters: a binding refines only a symbol its own function's walk
  added, and otherwise shadows with a fresh entry that the pop removes.
  `main` had never been popped at all, so its locals outlived it and
  shadowed everything walked after it; it now gets the same push and pop as
  any other function. `tests/integration/local_shadows_module_extern` binds
  `floor` four ways (a pointer local, an `int` local, a destructure target,
  a local in `main`) against a module reached through another module, and
  checks both an `int` and a `float` use of the extern.
