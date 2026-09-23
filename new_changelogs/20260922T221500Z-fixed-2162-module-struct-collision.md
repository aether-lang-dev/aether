- **A module's own struct no longer collides with a type the runtime injects
  (#2162).** `@c_include` put `std/collections/aether_arr_inline.h` into the
  translation unit of every program importing `std.intarr` / `std.floatarr`
  / `std.longarr`, so that element access inlines — and that header declared
  bare `struct IntArray`, `FloatArray` and `LongArray`. Common names, now
  claimed inside someone else's program: aephysics had carried its own
  `struct IntArray` since its first layer, and on 0.708.0 every program
  importing both stopped compiling with a C error naming neither module. A
  header the runtime injects declares only `Aether`-prefixed names now;
  `aether_collections.h` keeps the short aliases, because C includes that one
  deliberately rather than having it imposed.
  `tests/integration/module_struct_name_collision` builds a module's own
  `IntArray` beside `std.intarr`, and checks the rule against the header so
  a name added later is caught before it reaches anyone's program.
