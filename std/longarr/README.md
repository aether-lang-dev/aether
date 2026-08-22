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

## Exports

`new`, `new_filled`, `size`, `get`, `set`, `get_unchecked`, `set_unchecked`,
`fill`, `free`.
