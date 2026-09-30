# std.floatarr

A packed, fixed-size array of `float` (a double).

Same shape as `std.intarr` and `std.longarr`. Packed contiguous doubles are
what a numeric routine wants: samples, coordinates, a column of measurements.

```aether,run
import std.floatarr

main() {
    a, err = floatarr.new_filled(2, 1.5)
    println("size=${floatarr.size(a)} err='${err}'")

    value, gerr = floatarr.get(a, 1)
    println("a[1]=${value} err='${gerr}'")

    floatarr.free(a)
}
```
```output
size=2 err=''
a[1]=1.5 err=''
```

Float text here follows the C locale, so `1.5` prints with a point on every
platform — see `std.number` for locale-aware rendering.

`std.sort` sorts one in place with `sort.floats`.

**Indexing with `v[i]` (#2041).** `a[i]` on the handle itself cannot work —
the handle is a bare `ptr`, so `[]` has no element type to dispatch on. A
**view** is typed, and does:

```aether,fragment
a = floatarr.floatarr_new_raw(n)
v = floatarr.floatarr_array(a)      // a `float[]` over the same buffer
v[3] = 1.5
total = total + v[3]
```

The view *is* the buffer, not a copy, so writes through it are writes to
the array and the accessors see them (and vice versa). It is bounded by the
array's size (#2301): `v.len` is `floatarr.size(a)`, and `v[i]` outside it is
the slice bounds panic rather than a read past the buffer. It passes to any
`float[]` parameter, std.sort included (`sort.floats(v)`, or
`sort.floats(v[..n])` for a prefix). A null handle is the empty slice. It
borrows: valid until the handle is freed. `floatarr_get_unchecked` is still
the load with no check, for a hot loop that has proved its own bounds.

## Exports

`new`, `new_filled`, `size`, `get`, `set`, `get_unchecked`, `set_unchecked`, `floatarr_data`, `floatarr_array`,
`fill`, `free`.
