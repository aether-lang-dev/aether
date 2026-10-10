- **A program's top-level `var` no longer leaks into its imported modules.**
  A module function's bare `ok = ...` binds its own local, but the checker
  and codegen resolved it to the importing program's `var ok`: since 0.801
  (#2613 checks every function of every module) `var n = 0` beside
  `import std.fs` failed to type-check inside std.fs (`n = fs_pwrite_raw`,
  E0200), and where the types agreed the module silently wrote the
  program's variable (`var ok = 42` then `fs.delete(...)` left `ok == 0`,
  as on 0.791 and earlier). Lookups from a merged module body now skip the
  program's own globals in the checker, the inference pass and codegen; a
  module's own globals and locals, and the program's, stay apart.
  `tests/integration/program_global_not_in_module_scope`;
  `asks/REPLY-aether-program-global-leaks-into-module-scope.md`.
