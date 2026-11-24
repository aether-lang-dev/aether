- **`import m (*)` binds what `m` exports, not its private `var`s.** The
  glob bound every name of the module, a private module-level `var text`
  included, so an importer's own loop variable or local `text` was the
  module's cell (`m_text = <a string>` on a `string[]`, failing in the C,
  or a silent write to the module's state). A glob now binds only what
  the module's `exports (…)` list names (everything, without a list), in
  a program and in a module that glob-imports another, and a write to a
  module `var` is renamed to the module's cell only when the import binds
  that var.
