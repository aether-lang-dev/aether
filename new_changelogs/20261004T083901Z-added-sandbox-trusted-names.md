- **Trusted names in `sandbox.enforce(perms, foo, db) { … }`.** Inside an
  enforced block every sandbox check applies to whatever code makes it,
  including functions defined before the block. Names after the grant list
  are exempt: a call written in the block to a named function, or into a
  named module, runs with the authority of the code that wrote the
  `enforce`. The exemption cannot be claimed by contained code (an `enforce`
  it writes is entered already sandboxed), is lexical (a helper that calls
  `foo` from elsewhere stays sandboxed, and the names cannot be used as
  values in the block), and leaves the trusted call's arguments evaluated
  inside the sandbox. Each trusted Aether function gets a generated wrapper
  with its exact signature; C externs cannot be trusted, and every misuse is
  a compile error that says what is wrong.
