# std.sort

In-place ascending sort and binary search over slices: `int[]`, `long[]`,
`float[]` and `string[]`.

Every entry point takes a slice, so the length travels with the elements and
no call takes a separate count. A whole array sorts as `sort.ints(a)`, a
prefix as `sort.ints(a[..n])`, any run as `sort.ints(a[lo..hi])`. The packed
arrays hand over their elements as a bounded slice (`intarr.array(h)`,
`longarr.array(h)`, `floatarr.array(h)`, `strarr.array(h)`), and a `T[N]`
array, a `make([]T, n)` buffer or a bounded view of C memory
(`(p as int[])[0..n]`) passes as it is. A view with no bound (`p as int[]`,
whose `.len` is -1) has no length to sort, and panics.

`*_search` requires an already-sorted slice. It returns the index of the
first element not ordered before `x`: `x`'s index when present, otherwise
the insertion point (`xs.len` when every element sorts before it). Searching
an unsorted slice is not an error, it just gives a meaningless answer, so
sort first.

```aether,run
import std.sort
import std.intarr

main() {
    a, err = intarr.new_filled(5, 0)
    v = intarr.array(a)
    v[0] = 5
    v[1] = 3
    v[2] = 9
    v[3] = 1
    v[4] = 7

    sort.ints(v)
    println("sorted: ${v[0]} .. ${v[4]}")
    println("index of 7: ${sort.int_search(v, 7)}")

    fixed = [4, 2, 8, 6]
    sort.ints(fixed[..2])
    println("prefix: ${fixed[0]} ${fixed[1]} ${fixed[2]} ${fixed[3]}")

    intarr.free(a)
}
```
```output
sorted: 1 .. 9
index of 7: 3
prefix: 2 4 8 6
```

## Strings

Ordering is `std.string.compare`: lexicographic **byte** order, binary-safe.
That matches the default in C, Go and Zig; locale-aware collation is a
separate concern and lives in `contrib/i18n`.

```aether,run
import std.sort
import std.string

main() {
    a = ["pear", "apple", "fig"]
    sort.strings(a)
    println("${a[0]} ${a[1]} ${a[2]}")
    println("fig at ${sort.string_search(a, "fig")}")
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
    sort.strings_by(a, descending)
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
