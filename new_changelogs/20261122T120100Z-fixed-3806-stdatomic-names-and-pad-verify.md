- **A function or variable named like a `<stdatomic.h>` name compiles.**
  Every generated program includes it, and a program's own
  `atomic_load(p: ptr) -> int` reached C under the name of its macro and
  did not compile; an imported module's function hid it behind the
  module's prefix, and `ae pad --verify`, which compiles a module alone,
  failed on it. Its generic functions, types and memory_order constants are
  now renamed as the other C library names are. `ae pad --verify` tells a
  failed assertion (the layout report disagrees with the compiler) from C
  that did not compile, and its summary names the files that failed
  (#3806).
