# REPLY: two modules with the same basename break each other's qualified types

**Re:** nib-code `asks/aether-module-basename-collision.md` (branch
`aether-port`)

**RESOLVED** in PR #PRNUM (branch `batch/nov13-naeb-asks`; the first
release after 0.913.0 carries it).

Cause, close to the ask's guess: when two loaded modules share a last
segment, the loader keeps them apart by giving each a namespace from its
full path (`a_timing`, `b_timing`). The merge resolves each file's
`timing.T` against that file's own imports by the written prefix (its alias
or the path's last segment) and finds the right struct. But the
typechecker's check of qualified types (`qual_resolve_module`) matched the
qualifier against the module's *namespace* and full path only, so with a
collision `timing` named nothing, in every file, and the program was
refused before the (correct) resolution was used. Qualified calls took the
written prefix since #2268; types did not.

The check now also takes the import path's last segment (and the file's own
module's), as the merge does. Each file's `timing.` means the module that
file imports.

`tests/integration/module_basename_qualified_types` is the ask's repro, with
both modules also exporting a `Rec` of different layouts, and module `c`
naming `timing.T` in a cast, a parameter, a struct field, a slice local and
a literal. On the release carrying this, nib can name its provider timing
module `nib.provider.timing` again (now `nib.provider.model_timing`).
