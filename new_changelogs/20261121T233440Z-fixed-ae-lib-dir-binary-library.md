- **`AETHER_LIB_DIR` finds a binary library by its file name, as `--lib`
  does.** The `ae` driver's binary-import prepass probes the search chain for
  `lib<name>.so` (`.dylib`, `.dll`), and the variable joined that chain only
  later, when the cache key was computed: a library built from a source of
  another name (RexxAE's `librexxae`, from `src/engine.ae`) was "unresolved
  import" under `AETHER_LIB_DIR` and found under `--lib`. In a project with
  `[dependencies]` the variable was ignored altogether, since the
  dependencies' directories in the chain counted as a typed `--lib`. It is now
  read once per command, before anything else joins the chain
  (`tc_lib_dirs_seed_from_env`), and every probe reads the same chain.
