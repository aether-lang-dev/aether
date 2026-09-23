- **`std.longarr`'s element type is `int64_t`, not `long long`.** Aether's
  `long` lowers to `int64_t`, which on LP64 (every Linux and macOS target)
  is `long` — and `long long*` and `long*` are *different* pointer types
  there even though both are 64 bits. The packed-array header declared the
  buffer as `long long*`, so the view accessor added for #2041 returned one
  type while the generated C expected the other: a warning on gcc ≤ 13 and
  a hard error on gcc 14 and Ubuntu 24.04. Windows hid it entirely, where
  `long` is 32-bit and `int64_t` is `long long`. The header, the `.c` and
  the collections header now all say `int64_t`.
