- **A builder's parameters and locals shadow its module's functions.** In a
  `builder` body a parameter or local named like a function of the same
  module resolved to the function, where a plain function's resolves to the
  local: `builder java_main(main_class: string)` beside a `main_class()`
  setter interpolated the setter's address (E0200), a local `with_path`
  compared the `with_path()` setter's address with 1, and a local `depth`
  passed the `depth()` setter's address as an int (aeb's java, bldr and
  fetch modules). The module-merge rename now enters a builder's scope as it
  does a function's. `asks/REPLY-builder-local-resolves-to-module-function.md`.
