- **`string.length` and `string.char_at` read the string in place (#3836).**
  They were out-of-line calls that checked the header's magic on every
  call, so `while i < string.length(s)` made two calls per character and
  the C compiler hoisted neither. std.string now binds them to `static
  inline` reads in `aether_string_inline.h`, which it asks for with
  `@c_include`; the std modules that declared the two names themselves
  bind the same reads. The semantics are the same: null is 0 long, an
  index out of range reads 0, a byte is 0..255, and a length past 2 GiB
  panics. `string_length` and `string_char_at` stay exported for C, with
  the same bodies. A scan of a 132-byte counted string went from about 400
  to 270 ns (the new `string_scan_char_at` row of benchmarks/core). C that
  compiles generated C with a hand-picked include list (rather than `ae
  cflags`) needs `std/string` on it now.
