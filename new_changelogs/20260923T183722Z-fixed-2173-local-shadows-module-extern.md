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
  added, and otherwise shadows with a fresh entry that the pop removes. The
  check is a per-walk stamp on the symbol, O(1), where #1967's list walk
  made rebinding quadratic in a function's locals.
  `main`'s locals were worse during inference: they stayed in the table
  after its walk, so for every walk after it a local of `main` stood in for
  any function, extern or local of the same name. A `p` that `main`
  destructured as a pointer was what a TLS function's own `p` resolved to
  until that function's first binding was typed, which had only been hidden
  by the retype bug above. `main` is now walked like any function, and its
  locals leave the table when the walk ends. They are put back once
  inference is done, because the typechecker still reads a local bound in an
  `if` arm or loop body of `main` from there. That also means a local in
  `main` named like a module's extern still trips the typechecker (#2186).
  `tests/integration/local_shadows_module_extern` binds `floor` three ways
  (a pointer local, an `int` local, a destructure target) against a module
  reached through another module, and checks both an `int` and a `float`
  use of the extern.
