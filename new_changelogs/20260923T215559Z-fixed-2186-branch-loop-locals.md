- **A local bound in an `if` arm or a `while` body can be read after the
  block in any function, not only in `main` (#2186).**

  ```aether
  show(c: int) {
      if c == 1 { heading = "A" } else { heading = "B" }
      println(heading)    // was: error[E0300]: Undefined variable 'heading'
  }
  ```

  Codegen always supported this. It declares such a local in an enclosing
  scope by three rules:
  - the arm locals of a function body's top-level `if`s that a top-level
    statement reads;
  - the names both arms of an `if/else` declare, at any depth;
  - a `while` body's locals, including those nested in it.

  But the typechecker gave every block its own scope, so the read after it
  resolved only through the names the early inference pass had left in the
  program table. It left them for `main`, whose locals it never popped, and
  never for any other function.

  The three rules now live in one place, `compiler/analysis/hoist.c`, which
  codegen and the typechecker both use. The typechecker declares each name
  where codegen declares it, with the same joined type, so a read the
  generated C allows is accepted and a read it would reject is an
  `Undefined variable` error from the typechecker, not a C compiler's
  "undeclared". Codegen output is unchanged: 425 of 427 regression tests
  and examples generate byte-identical C before and after. The other two
  are the float-fold test, whose change is intended, and one file that
  fails on both.

  With that, inference pops `main`'s locals like any function's, and they
  stopped leaking: a local in `main` no longer shadows or retypes a function,
  extern or local of the same name for everything checked after it. That
  completes #2173 for `main`. New test:
  `tests/integration/hoisted_local_scope`. The `main` case is back in
  `local_shadows_module_extern`.
