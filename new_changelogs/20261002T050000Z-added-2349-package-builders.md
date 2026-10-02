- **A package library exports its modules' builders.** A `builder`
  in a module of a package built with `ae build --emit=lib --package` is
  now exported, where before the build only warned that it was left out.
  The library exports a wrapper named after the module
  (`aether_ae3d_ui__panel`) with the builder's parameters and the trailing
  `void* _builder` config. The builder's catalog record carries its module
  (schema 1.6), and the interface `ae` builds for an `import` of that module
  declares it as a trailing-block builder, so `ui.panel("hud") { ... }`
  works against the prebuilt library (#2349).
