# std.sort

In-place ascending sort and binary search over the packed numeric arrays
(`std.intarr`, `std.longarr`, `std.floatarr`).

The array carries its own length, so neither the sort nor the search takes a
count — passing one is a type error rather than a silent mismatch.

`*_search` requires an already-sorted array: it returns the index of `x`, or a
negative value when absent. Searching an unsorted array is not an error, it
just gives a meaningless answer, so sort first.

```aether,run
import std.sort
import std.intarr

main() {
    a, err = intarr.new_filled(5, 0)
    intarr.set(a, 0, 5)
    intarr.set(a, 1, 3)
    intarr.set(a, 2, 9)
    intarr.set(a, 3, 1)
    intarr.set(a, 4, 7)

    sort.ints(a)

    first, _ = intarr.get(a, 0)
    last, _ = intarr.get(a, 4)
    println("sorted: ${first} .. ${last}")

    println("index of 7: ${sort.int_search(a, 7)}")

    intarr.free(a)
}
```
```output
sorted: 1 .. 9
index of 7: 3
```

## Exports

`ints`, `longs`, `floats`, `int_search`, `long_search`, `float_search`.
