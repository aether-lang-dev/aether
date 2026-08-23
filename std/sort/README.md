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

## Strings

`string[]` is a bare C array with no carried length, so the string entry
points take an explicit element count. Ordering is `std.string.compare`:
lexicographic **byte** order, binary-safe. That matches the default in C, Go
and Zig; locale-aware collation is a separate concern and lives in
`contrib/i18n`.

```aether,run
import std.sort
import std.string

main() {
    a = ["pear", "apple", "fig"]
    sort.strings(a, 3)
    println("${a[0]} ${a[1]} ${a[2]}")
    println("fig at ${sort.string_search(a, 3, "fig")}")
}
```
```output
apple fig pear
fig at 1
```

## Custom orders

The `_by` forms take a comparator returning `<0`, `0`, or `>0`. It must be a
strict weak ordering — an inconsistent comparator yields an unspecified
permutation rather than a crash, but not a sorted array either.

They are separate names rather than an optional argument so the default path
keeps a direct comparison instead of an indirect call per element.

```aether,run
import std.sort
import std.string

descending(a: string, b: string) -> int {
    return 0 - string.compare(a, b)
}

main() {
    a = ["pear", "apple", "fig"]
    sort.strings_by(a, 3, descending)
    println("${a[0]} ${a[1]} ${a[2]}")
}
```
```output
pear fig apple
```

## Exports

`ints`, `longs`, `floats`, `int_search`, `long_search`, `float_search`,
`strings`, `string_search`, `ints_by`, `longs_by`, `floats_by`, `strings_by`.

All sorts are IN PLACE and none is stable.
