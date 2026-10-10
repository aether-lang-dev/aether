# REPLY: a program's top-level `var` leaks into an imported module's function bodies

**Status:** RESOLVED in branch `fix/release-contrib-archives-builder-scope`
(the PR that carries this file). **Ask:** aether-ui
`asks/aether-program-global-leaks-into-module-scope.md` (2026-10-10).

## Cause

Two halves, one older than the other.

- **The silent write is old.** A module function merged into a program is
  emitted in the program's translation unit, where the program's `var ok`
  is a file-scope static that codegen records as a global; a bare
  `ok = file_delete_raw(path)` in std.fs's `delete` was lowered as a store
  to it instead of a local declaration. 0.791.0 already printed `ok=0` for
  your second program; nothing checked it.
- **The type-check failure is new in 0.801.** Bisected (`n` case, by
  `aetherc` alone) to 06fc0068 "Check every function of every module, and
  only what a module imports (#2613, #2614)": every merged function is now
  type-checked, so std.fs's `pwrite`, which nothing in your program calls,
  had its `n = fs_pwrite_raw(...)` (64-bit) checked against the program's
  32-bit `var n` (E0200). Before, an uncalled function was pruned unchecked.

## Fix

A module's own globals are merged under `<ns>_name`, so from a merged body a
bare name that finds a file-scope `var` can only have found the program's.
Such a lookup now skips the program's own top-level `var`s (no
`origin_module`):

- typechecker `lookup_symbol`: from a scope whose `merged_from` is set, a
  program global is not resolved (the inference pass shares the table, so
  its locals, stamped with a walk id, are not affected);
- codegen `is_module_global_var`: a program global is not a global inside a
  function emitted from a module (`current_origin_module`), so the module's
  `ok` is declared as its own local, which C scoping keeps apart.

The program's own code sees its globals as before, and a module's own
globals (`var hits` in a module) are unaffected.

## Test

`tests/integration/program_global_not_in_module_scope`, all three failing on
main (0.802.0, 7adbe94e):

- `var n = 0` + `import std.fs` must build and print `n=1` (E0200 before);
- `var ok = 42` + `fs.delete(...)` must print `ok=42` (`ok=0` before);
- a test module whose function's local `count`, a closure-captured local
  `total`, and its own global `hits` share names with the program's globals:
  the program's values must be unchanged (`count` was overwritten before).

## For aether-ui

The `undo_total_` renames stay correct; the original names work again from
the release carrying this.
