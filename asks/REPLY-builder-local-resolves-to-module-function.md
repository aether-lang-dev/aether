# REPLY: inside a `builder` body, a local or parameter named like a module function resolves to the FUNCTION

**Status:** RESOLVED in branch `fix/release-contrib-archives-builder-scope`
(the PR that carries this file). **Ask:** aeb
`asks/builder-local-resolves-to-module-function.md` (2026-10-09).

## Cause

Not the checker: the module-merge pass. When a module is merged into a
program, `rename_intra_module_refs` (compiler/aether_module.c) rewrites every
bare reference to one of the module's functions to its `<module>_<name>`
spelling, and skips the names a local binding shadows. It collects those
bindings (parameters and locals) when it enters an `AST_FUNCTION_DEFINITION`
or an `AST_CLOSURE`, but a builder is an `AST_BUILDER_FUNCTION`, so a
builder body entered no scope: its `main_class` parameter, `with_path` local
and `depth` local were all renamed to the module's setters. The typechecker
had resolved them to the locals, which is why only the E0200 interpolation
check (which looks at the renamed AST) ever said anything.

## Fix

`AST_BUILDER_FUNCTION` enters a scope wherever `AST_FUNCTION_DEFINITION`
does: in `rename_intra_module_refs` and in the two qualified-prefix rewrites
(`from.x` alias renames) that share the same scope rule. Parameters and
locals now shadow a same-named module function in a builder body exactly as
in a plain function; calls to module functions that are not shadowed are
still renamed.

## Test

`tests/integration/builder_local_shadows_module_fn`: one module per shape,
because the interpolation case rejected the whole module --

- `bs_param`: a parameter passed on and interpolated (aeb `java.java_main`);
  before, E0200.
- `bs_cmp`: a local assigned, then compared with 1 (aeb
  `bldr.install_launcher`); before, the address was never 1, so it printed
  "PATH untouched".
- `bs_int`: a local passed as an int (aeb `fetch.git`); before, a C error
  under clang (`-Wint-conversion`), a truncated address under gcc.

Each checks the program's output and that the emitted C names the local, not
`<module>_<name>`. All three fail on main before the fix.

## For aeb

The renames aeb made stay correct. Once aeb's floor reaches the release
carrying this, a builder can use the natural names again; nothing needs to
change before then.
