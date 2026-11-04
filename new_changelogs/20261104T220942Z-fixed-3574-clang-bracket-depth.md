- **Programs nesting blocks deeply build with Clang (#3574).** Blocks
  nest in the C as deep as in the source, and the parser takes them 512
  levels deep, but Clang refuses C nested past 256 brackets unless told
  otherwise: about 128 nested `match` arms or 85 nested `for` loops did not
  build on macOS. `ae` now passes `-fbracket-depth=2048`, sized from the
  parser's own limit, to every Clang it drives (`ae run`, `ae build`,
  cross builds through zig cc or xcrun, the wasm backend through emcc,
  and `$AE_CC` / `$CC` naming Clang anywhere). The build system guide
  lists which C compilers fix a nesting limit; MSVC's, about 100 to 110
  nested blocks, cannot be raised.
