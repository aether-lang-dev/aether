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

## Exports

`new`, `new_filled`, `size`, `get`, `set`, `get_unchecked`, `set_unchecked`,
`fill`, `free`.
