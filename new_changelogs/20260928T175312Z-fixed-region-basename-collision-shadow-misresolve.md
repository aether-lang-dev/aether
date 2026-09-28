- **Two same-last-segment modules imported unaliased in one scope no longer
  let the first one's rewrite claim every qualified call the other's meant,
  and a same-named local parameter no longer blocks the rewrite entirely.**
  `rewrite_import_prefixes` walked a scope's imports one at a time, rewriting
  `written.name` to the imported module's collision-assigned namespace
  (#2209); when TWO of a scope's imports reduced to the same written prefix
  (`vg.geom.region` selectively imported, `vg.region` imported unaliased,
  both renamed off "region" by module_assign_namespaces), the first import
  processed rewrote every `region.name` in the body — including the second
  import's own qualified calls — to ITS namespace, so the second import's
  calls resolved against a module that never defined them
  ("'region_set_rect' is not exported from module 'vg_geom_region'", or an
  outright wrong-argument-count error against a same-named function on the
  wrong side). Fixed by grouping a scope's imports by their written prefix
  first: a prefix only one import writes still rewrites the simple way, and
  a prefix two or more share is resolved per occurrence, by which candidate
  module actually exports that specific suffix.

  A second, related bug hit any of these same-named-as-their-module
  parameters: `rewrite_qualified_prefix` skipped rewriting a `from.name`
  reference entirely whenever `from` was also bound as a parameter or local
  in scope, on the theory that `x.field` on a local struct is a real field
  read rather than a qualified module call. That is true for
  `AST_MEMBER_ACCESS`, but `AST_FUNCTION_CALL` and `AST_IDENTIFIER` only ever
  carry a dotted value from literal qualified-call syntax the parser already
  committed to — Aether has no method-call sugar that would let a local
  value change what `from.name(...)` means — so skipping those two on a
  same-named local left a same-named forwarding wrapper
  (`region_set_animated(region: ptr, on: int) { region.region_set_animated(region, on) }`,
  a real pattern in aether-ui) with its own call unrewritten and misresolved.
  Those two node types now rewrite unconditionally; `AST_MEMBER_ACCESS`
  keeps the shadow guard, but now yields to it only when the target module
  doesn't export a member of that name — `region.LIVE_RASTER` through the
  same `region: ptr` parameter is a qualified constant reference an opaque
  pointer could never have as a real field, and now resolves as one.

  Found rebuilding aether-ui's `apps/frames_demo` (and its own
  `ui/frames.ae`, `vg/live.ae`, `vg/module.ae`) against a from-source
  `main`; a minimal two-module repro reproduces it without aether-ui. The
  existing module/namespace collision integration suite
  (`module_leaf_collision`, `std_leaf_collision`, `import_export_collision_reject`,
  `namespace_multifile`, `namespace_basic`, `sealed_namespaces`,
  `module_struct_name_collision`, `many_namespaces_qualified_call`) stays
  green, and the full integration suite is unaffected (427/433; the other 6
  fail only for a pre-existing missing-TLS-backend reason in this dev build,
  unrelated).
