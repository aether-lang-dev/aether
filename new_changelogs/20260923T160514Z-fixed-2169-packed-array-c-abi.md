- **The packed arrays' unchecked accessors are exported from libaether
  again, as well as being inline (#2169).** #1986 made
  `intarr_get_unchecked` and its siblings `static inline` in
  `aether_arr_inline.h` so element access vectorises, and in the same change
  removed their out-of-line definitions. But those names were part of
  libaether's ABI, and C outside the standard library declares and calls
  them without the header — aether-ui's GTK4 and Win32 backends do
  `extern double floatarr_get_unchecked(void* arr, int i);`. From 0.708.0
  every program linking the toolkit failed with `undefined reference to
  floatarr_get_unchecked`, ae3d's editor among them.

  Each accessor now has two definitions, on purpose: a translation unit
  that includes the header still gets the inline body, and the three
  `.c` files define `AETHER_ARR_NO_INLINE_ACCESSORS`, see prototypes
  instead, and export real definitions under the same names — ordinary C,
  since a `static inline` in one TU and an external definition in another
  never meet. `tests/integration/packed_array_c_abi` links a C program that
  only *declares* the accessors, the way aether-ui does; checks every
  accessor the header declares against libaether's exported symbols, so
  one added later without its twin is caught here; and compiles a TU that
  includes the header to confirm it still gets a load rather than a call,
  so the vectorisation win is not quietly given back to fix the link.
