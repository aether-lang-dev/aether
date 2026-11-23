- **A program's compile line names only the header directories it needs
  (#3841).** `ae` passed every runtime and std directory that holds a
  header, 53 in a source tree, as `-idirafter`, where hello needs 3: the
  compiler searched them all for each header. `ae` now follows the
  generated C's includes (and those of the headers they reach, the
  module `@c_include` headers and the `@source` files) and keeps the
  directories they resolve in, in the list's order and still after the
  system's (#2552). Anything it cannot follow keeps every directory, as
  before: an include through a macro, an `#include_next`, flags that move
  the search in the cflags or in the `$AE_CC` / `$CC` command itself, and
  CPATH, C_INCLUDE_PATH or OBJC_INCLUDE_PATH set. Header names match in any
  case as well as exactly, so an include spelled in another case on Windows
  or macOS keeps its directory. Hello's preprocessed C and object are byte
  for byte the same.
