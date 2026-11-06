- **`ae test` and `ae check` resolve `[dependencies]` and `[patch]`.** Only
  `ae build` and `ae run` put a project's dependencies on the module search
  path, so a project that built and ran against a package (`ae add
  github.com/aether-lang-dev/aether-guest-languages`, then `import lua`) had
  every test importing it fail with "FAIL (compile)" and `ae check` refuse the
  program, the import unresolved. Both commands now go through the same
  resolver, from any directory of the project.
