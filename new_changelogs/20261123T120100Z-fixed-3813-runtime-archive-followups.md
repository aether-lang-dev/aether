- **The runtime compiled from source: four fixes to #3813's cache of it.**
  Under `-flto` the cached runtime archive was made with plain `ar`, which
  gives GCC's slim LTO objects no symbol index: the link failed, and the
  archive was published, so every later build failed too. It is made by
  the compiler's LTO-aware archiver now (`gcc-ar`, `llvm-ar`, `zig ar`), and
  no archive is published, or linked, without a symbol index naming its
  symbols; when no archiver makes one, the objects are linked. An archive
  the cache would not take (`<slot>.tmp.<pid>`) is removed once the build is
  done; it stayed for good, as the size cap skips temporaries, and a slot
  path holding a directory now counts as a failed publish rather than a
  published one. The program's cache key folds the runtime's sources when
  the build compiles the runtime from source (`--trace`, no archive, or an
  archive on another C runtime, by the decision the build makes, which is
  remembered for the next build's key, a compiler that cannot be asked
  included), with the runtime's source list (`build/MANIFEST`), so an edit
  to a `runtime/*.c` file in a source tree no longer leaves `ae run`
  serving the program built before it; a build that links the archive,
  with cflags or without, walks the headers only, as before. And the C
  runtime probe's control compile takes the project's cflags as the probe
  does, and the probe text is warning-clean (it
  declares what it defines, and its `#error` lines hold no apostrophe):
  `-Werror` alone, or `-Werror -Wmissing-variable-declarations`, made the
  archive's own compiler "target another C runtime", and the whole runtime
  was compiled from source.
