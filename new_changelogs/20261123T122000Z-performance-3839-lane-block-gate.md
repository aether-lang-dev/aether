- **A program that uses no lanes no longer carries the std.lanes helpers
  (#3839).** Every generated program held about 585 lines of lane helpers
  and `#include <emmintrin.h>`, whatever it used. They now go only into a
  program that has a lane type, imports a module at a `@target`, or has an
  extern naming one of the `_ae_` lane helpers (`lanes.native8()`,
  `lanes.cpu_has_avx2()`). Hello's preprocessed C went from 8,783 to 4,540
  lines and its compile from about 144 to 130 ms on Windows.
