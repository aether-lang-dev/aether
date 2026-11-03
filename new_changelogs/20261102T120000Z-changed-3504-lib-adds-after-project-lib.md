- **`--lib` and `AETHER_LIB_DIR` add to the module search path instead of
  replacing the project's `lib/` (#3504).** A program that imports a module
  of its own `lib/` and a library from elsewhere used to need both named
  (`--lib lib --lib ../other`); it now names only the other. The project's
  `lib/` is searched first, then the `--lib` directories (or
  `AETHER_LIB_DIR` when no `--lib` is given), then the dependencies, the
  same in `ae` and `aetherc`. `ae lib-path` prints that chain, `./lib`,
  `lib/` and `lib` are one entry, and editing a module under `lib/` now
  rebuilds a cached program that was built with `--lib` too.
