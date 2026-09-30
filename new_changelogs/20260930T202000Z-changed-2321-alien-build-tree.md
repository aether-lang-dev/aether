- **Cross builds go to `build/.alien/<target>/`; the native build stays in
  `build/`.** A `WINDOWS=1` or `FREEBSD=1` build and a native build used to
  share `build/`, and the Makefile refused the second until `make clean`, so
  every switch was a full rebuild. Every Makefile path now goes through
  `$(BUILD_DIR)`, which is `build` for the native build and
  `build/.alien/windows-x86_64` / `build/.alien/freebsd-x86_64` for a cross
  build: both stay warm and alternating rebuilds nothing. Native paths, the
  tests and `ae` are unchanged; the Windows and FreeBSD cross legs and the
  FreeBSD release packaging read the cross tree. `make clean` removes both
  (#2321).
