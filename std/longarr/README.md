# std.longarr

A packed, fixed-size array of `long` (64-bit signed).

Same shape as `std.intarr`, eight bytes per element instead of four. Reach for
it when the values exceed 32 bits — timestamps in milliseconds, byte offsets
into a large file, accumulated counters — where an `int` array would silently
wrap.

```aether,run
import std.longarr

main() {
    a, err = longarr.new_filled(3, 7)
    println("size=${longarr.size(a)} err='${err}'")

    value, gerr = longarr.get(a, 0)
    println("a[0]=${value} err='${gerr}'")

    longarr.set(a, 0, 9007199254740993)
    big, _ = longarr.get(a, 0)
    println("a[0]=${big}")

    longarr.free(a)
}
```
```output
size=3 err=''
a[0]=7 err=''
a[0]=9007199254740993
```

That last value is past 2^53, where a double-backed number would start losing
integer precision — a packed long array does not.

`std.sort` sorts one in place with `sort.longs`.

**Indexing with `v[i]` (#2041).** `a[i]` on the handle itself cannot work —
the handle is a bare `ptr`, so `[]` has no element type to dispatch on. A
**view** is typed, and does:

```aether,fragment
a = longarr.longarr_new_raw(n)
v = longarr.longarr_array(a)      // a `long[]` over the same buffer
v[3] = 42
total = total + v[3]
```

The view *is* the buffer, not a copy, so writes through it are writes to
the array and the accessors see them (and vice versa). `v[i]` lowers to the
same load `longarr_get_unchecked` inlines to, so the readable spelling
costs nothing. It borrows: valid until the handle is freed, and bounds are
yours to respect, exactly as for the unchecked accessors.

## Exports

`new`, `new_filled`, `size`, `get`, `set`, `get_unchecked`, `set_unchecked`, `longarr_data`, `longarr_array`,
`fill`, `free`.
