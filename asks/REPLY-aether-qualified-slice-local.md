# REPLY: a typed local of a qualified struct's slice type does not parse

**Re:** nib-code `asks/aether-qualified-slice-local.md` (branch
`aether-port`)

**RESOLVED** in PR #PRNUM (branch `batch/nov13-naeb-asks`; the first
release after 0.913.0 carries it).

Cause: the statement dispatcher recognises a typed local by its shape before
handing it to `parse_variable_declaration` (whose `parse_type` already reads
`m.P[]`). It knew `Type name`, `Type[N] name` and `mod.Type name` (#946),
but not `mod.Type[...] name`, so the statement went to the expression parser,
which read `m.P` as a field and stopped at the `[`.

Now each `[...]` on the type's line after a dotted type name is part of the
type when a binding name follows it: `m.P[] xs = []` and `m.P[4] ps = [...]`
are declarations. An element read (`a.b[i]`) is never followed by a name on
its line, so expression statements are unaffected.

`tests/integration/qualified_type_name` now declares `lib.Box[] xs = []` and
`lib.Box[2] pair = [...]`. On the release carrying this, nib can write
`m.P[] xs = []` where it now writes `make([]m.P, 0)` or calls
`commands.no_choices()`.
