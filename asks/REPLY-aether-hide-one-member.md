# REPLY: `hide ns.member` and `import ns hiding (a, b)`

**Re:** sae `asks/aether-hide-one-member.md`

**RESOLVED** in branch `fix/sae-asks-batch` (the first release after
0.909.0 carries it). Both forms the ask offered:

1. `hide ui.open_url` in a block or function: that member is refused
   (`E0304`) at any depth below, through an import alias (`import ui as u`
   then `u.open_url`), and taken as a value (`f = ui.open_url`). Every other
   `ui.*` stays visible. A value bound to `ui` at or below the hiding scope
   makes `ui.open_url` that value's field, which the hide leaves alone, as
   `hide ui` already did.
2. `import ui hiding (open_url)` at file level (also `import ui as u hiding
   (open_url)`): each body the file defines (function, method, receive arm)
   is held as if it began with `hide ui.open_url`. It is the importing
   file's rule: in a module it binds that module's code, not the program
   that imports the module, so sae's kernel can import `ui hiding
   (open_url)` while `services/shell` imports `ui (open_url)`. It does not
   combine with a selective or glob list.

What it does not cover: expressions outside any body (a top-level constant's
initializer, an actor's `state` initializer) are not held by the file-level
form; nothing in sae's kernel names `ui.open_url` there, and a `hide` in a
block never reached them either.

Two things changed for every `hide` on the way, found while testing this:

- A hidden qualified call is reported as hidden (`E0304: 'ui.open_url' is
  hidden in this scope`) where it used to be `E0301: Undefined function`.
- `hide ui` (the whole namespace) did not refuse a member taken as a value
  or a module constant (`f = ui.window`, `ui.SOME_CONST`): that path
  resolved the member without asking. It does now, for `hide` and
  `seal except` alike.

Tests: `tests/integration/hide_seal_directives/test_hide_reject.sh`, 13 new
cases (each refusal must name the hidden member, so a compiler that cannot
parse the case does not pass it); 12 of them fail on 0.909.0. Docs:
`docs/hide-and-seal.md` ("One member of a module") and
`docs/language-reference.md` (imports, scope directives).

For sae: once a release carries it, the kernel's `import ui` becomes
`import ui hiding (open_url)`, and `tests/check_layers.sh`'s open_url grep
can go (or stay as a second check of the import graph).
