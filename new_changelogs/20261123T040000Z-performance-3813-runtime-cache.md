- **The runtime `ae` compiles from source is built once per compiler
  (#3813).** A build that cannot link `libaether.a` (a compiler on another C
  runtime, or `--trace`) compiled the 119 runtime and std sources for every
  new program, 10 to 47 s each on Windows. It is now kept in the build cache
  as an archive, keyed by the compiler's identity, the flags and the content
  of the runtime and std sources, and every later program links it: with the
  WinLibs GCC `ae` downloads, the second program builds in 0.43 s where the
  first takes 29 s. `ae build -v` names it, `(built)` or `(hit)`. Two first
  builds at once each publish whole, as every cache slot is. On Windows
  `libaether.crt` records the archive's mingw-w64 major on a second line, and
  a compiler on another mingw-w64 compiles the runtime from source too (the
  same C runtime by name with other import libraries did not link); an `ae`
  that reads only the first line behaves as before.
