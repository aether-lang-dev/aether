- **`libaether.a` is linked only by a compiler on its own C runtime
  (#3813).** The archive is compiled against one C runtime's headers and
  imports its symbols: on Windows, `AE_CC="zig cc"` (UCRT) could not link
  any program that used `try` against the MINGW64 (msvcrt) archive,
  "undefined symbol: __declspec(dllimport) _setjmp". `make` now records the
  archive's C runtime in `libaether.crt` beside it (and installs and ships
  it there), from `runtime/aether_crt_id.h`; `ae` asks the build compiler
  through the same header, remembering the answer by the compiler's
  identity, and when the runtimes differ compiles the runtime from source
  with that compiler, as `--trace` does (`ae build -v` says why). An archive
  with no record is linked as before. The WinLibs GCC `ae` downloads on
  Windows when no gcc is on PATH (UCRT, while the releases' archive is
  msvcrt) takes the same fallback, and its zip is now checked against a
  pinned SHA-256 before it is unpacked; nothing checked it before.
