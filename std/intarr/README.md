# std.intarr

A packed, fixed-size array of `int`.

Packed means the values sit contiguously with no per-element boxing, so a
million ints cost four megabytes and iterate at memory speed. Fixed-size means
the length is set at construction and carried with the array — which is why
`std.sort` takes no separate count.

Accessors return `(value, err)`: an out-of-range index is an error you can
see, not a silent read of whatever was next in memory.

```aether,run
import std.intarr

main() {
    a, err = intarr.new_filled(3, 0)
    println("created err='${err}' size=${intarr.size(a)}")

    intarr.set(a, 0, 10)

    value, gerr = intarr.get(a, 0)
    println("a[0]=${value} err='${gerr}'")

    // Out of range is reported rather than read.
    _, oob = intarr.get(a, 99)
    println("a[99] err='${oob}'")

    intarr.free(a)
}
```
```output
created err='' size=3
a[0]=10 err=''
a[99] err='index out of range'
```

`get_unchecked`/`set_unchecked` skip the bounds test for hot loops. They must
behave identically in range — the only difference is what happens when you are
wrong, which is undefined rather than reported.

`std.longarr` and `std.floatarr` are the same shape for `long` and `float`.

## Exports

`new`, `new_filled`, `size`, `get`, `set`, `get_unchecked`, `set_unchecked`,
`fill`, `free`.
