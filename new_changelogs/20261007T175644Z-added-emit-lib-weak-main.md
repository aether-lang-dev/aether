- **A library build of a program links into an executable with no C
  `main()`.** `--emit=lib` (and obj, staticlib, csrc) kept a program's
  `main()` as `aether_main` / `aether_main_exit` (#2489) but emitted no C
  entry, so any tool linking the object into a program — aeb's `c.program`
  with an `aether_source` that defines `main()`, a Makefile, a bare
  `cc app.o $(ae cflags --libs)` — failed with `_main` undefined, and each
  consumer had to hand-write the same three-line C file (sae's
  `src/sae_entry.c`; aeb `asks/c-program-aether-source-main-entry.md`). The
  generated C, objects and static libraries now also define the executable's
  entry as a **weak** `main()` that runs `aether_main` then `aether_main_exit`
  and returns main()'s result. A host with its own `main()` keeps it (a strong
  definition wins). A shared library `ae` links itself still exports no
  `main`; neither does wasm, where emscripten would run it on load; and
  `-DAETHER_NO_LIB_MAIN` turns it off. See docs/emit-lib.md
  (tests/integration/emit_lib_keeps_main, checks 1b and 9).
