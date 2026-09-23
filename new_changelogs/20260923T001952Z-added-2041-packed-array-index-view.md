- **`v[i]` on a packed array, through a typed view (#2041).**
  `intarr.intarr_array(a)` returns an `int[]` over the same buffer (and the
  `floatarr` / `longarr` twins a `float[]` / `long[]`), so an array-heavy
  loop reads `v[3] = 42` instead of
  `intarr.intarr_set_unchecked(a, 3, 42)`. The view **is** the buffer, not
  a copy: writes through it are writes to the array, and the accessors see
  them in both directions.

  The issue recorded this as blocked — `[]` on the handle cannot dispatch,
  because the handle is a bare `ptr` with no element type, and the general
  fix was taken to be a distinct handle type threaded through three module
  APIs and every existing caller. It is not needed. A view is typed, so
  `[]` already works on one, and `std.strarr` has had exactly this shape
  since it was written (`strarr.array` feeding `sort.strings_by`).

  `v[i]` lowers to the same load `*_get_unchecked` inlines to after #1986,
  so the readable spelling costs nothing — a point the test asserts, since
  a view accessor that became an out-of-line call would still "work" while
  giving back everything that change bought. The view borrows: valid until
  the handle is freed, with bounds the caller's to respect, exactly as for
  the unchecked accessors.
