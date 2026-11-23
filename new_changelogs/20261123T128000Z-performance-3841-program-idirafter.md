- **A program's compile line names only the header directories it needs
  (#3841).** `ae` passed every runtime and std directory that holds a
  header, 53 in a source tree, as `-idirafter`, where hello needs 3: the
  compiler searched them all for each header. `ae` now follows the
  generated C's includes (and those of the headers they reach, the
  module `@c_include` headers and the `@source` files) and keeps the
  directories they resolve in, in the list's order and still after the
  system's (#2552). Anything it cannot follow, such as an include through
  a macro or cflags that move the search, keeps every directory, as
  before. Hello's preprocessed C and object are byte for byte the same.
