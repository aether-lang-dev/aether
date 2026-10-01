- **A dependency's own dependencies resolve for the project that uses it.**
  `ae` used to read only the project's manifest, so `app -> pkga -> pkgb`
  failed with an unresolved import from `app`. The consumer had to declare
  `pkgb` itself and patch it to a path inside `pkga`'s checkout. `ae run`,
  `ae build` and `ae lib-path` now walk the graph. A dependency's `[patch]`
  paths resolve against that dependency's own root, and the project's
  `[patch]` wins for any package in the graph. A package that resolves to
  two different places is an error naming both paths and who required
  each, and a missing transitive dependency names the package that needs it
  (#2335).
