- **A package of modules builds into one library.** `ae build --emit=lib
  --package ae3d` builds every module under the package's directory
  (`src/ae3d/core/module.ae` is `ae3d.core`) into one library, `libae3d`.
  Each module's public functions and constants are exported, meaning the
  names in its `exports` list, or all of them when it has no list, minus
  `_`-suffixed privates. Each function's C symbol is named after its module
  (`aether_ae3d_core__model_new`), so the symbol stays the same whichever
  other modules a build loads. The catalog (schema 1.4) records the module
  each export belongs to. A program's `import ae3d.core` with no source on
  the path now resolves to the library whose catalog lists that module, so
  `core.model_new(...)` works against the prebuilt library as it does
  against source. A library also stops cataloging constants it merely
  imported from other modules (#2297).
