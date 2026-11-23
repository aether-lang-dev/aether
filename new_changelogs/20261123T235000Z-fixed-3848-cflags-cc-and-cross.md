- **`ae cflags --cc <compiler>`, and GCC's relation limit on cross builds.**
  `ae cflags` printed the flags one compiler needs and another refuses
  (Clang's bracket depth, GCC's `--param relation-block-limit=0`) for the
  compiler `ae` drives, `$AE_CC`, `$CC` or the `gcc` on PATH, so a build
  that compiles generated C with Clang by its own means got GCC's
  parameter. `--cc` names the compiler the build uses, and that one is
  asked; the help says which is asked without it. `ae build
  --target=<triple>` now asks its cross compiler as the native build asks
  its own and passes the relation limit (and, optimising, the inlining
  budget) when it is a GCC that takes them: the float comparison folded
  wrong beside a NaN is a GCC cross compiler's bug too. `zig cc` and
  `xcrun clang` are Clang and get neither (#3848).
