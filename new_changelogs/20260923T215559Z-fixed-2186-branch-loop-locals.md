- **A local bound in an `if` arm or a `while` body can be read after the
  block in any function, not only in `main` (#2186).**

  ```aether
  show(c: int) {
      if c == 1 { heading = "A" } else { heading = "B" }
      println(heading)    // was: error[E0300]: Undefined variable 'heading'
  }
  ```

  Codegen always supported this: it hoists such a local to the enclosing
  scope (`hoist_if_branch_vars` for the arms of a function body's
  top-level `if`s, `hoist_loop_vars` for a `while` body). But the
  typechecker gave every block its own scope, so the read after it resolved
  only through the names the early inference pass had left in the program
  table. It left them for `main`, whose locals it never popped, and never
  for any other function. The typechecker now binds these names in the
  scope codegen hoists them to, with the numeric type joined across
  bindings as the hoisted declaration has it. Where codegen does not hoist
  (an `if` nested in another block), reading after the block stays an
  `Undefined variable` error from the typechecker, not a C compiler error.

  With that, inference pops `main`'s locals like any function's, and they
  stopped leaking: a local in `main` no longer shadows or retypes a function,
  extern or local of the same name for everything checked after it. That
  completes #2173 for `main`. New test:
  `tests/integration/hoisted_local_scope`. The `main` case is back in
  `local_shadows_module_extern`.
