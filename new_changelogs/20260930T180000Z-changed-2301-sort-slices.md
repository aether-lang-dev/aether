- **`std.sort` takes slices; the packed arrays' `array()` views are
  bounded.** `intarr.array`, `longarr.array`, `floatarr.array` and
  `strarr.array` return their elements bounded by the size, so `v.len` is
  the size and `v[i]` past the end is the slice bounds panic instead of a
  read past the buffer; a null handle is the empty slice. Every `std.sort`
  entry point takes a slice and no separate count: `sort.ints(intarr.array(h))`,
  `sort.strings(words)`, `sort.strings_by(xs[..n], cmp)`, and a fixed array,
  a `make` buffer or any sub-slice sorts the same way. A view with no bound
  panics rather than sorting nothing. Migration: `sort.ints(h)` becomes
  `sort.ints(intarr.array(h))` (likewise longs, floats and the `_by` and
  `_search` forms), and `sort.strings(a, n)` becomes `sort.strings(a[..n])`.
  Passing a container handle where a slice is expected now names its
  `array()` view in the diagnostic (#2301).
