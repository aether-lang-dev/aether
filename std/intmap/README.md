# std.intmap

A hash map from `long` keys to `long` values.

`std.map` keys every entry by a string. Looking up an integer there means
rendering it to text (an allocation), hashing the bytes, and comparing them
with `memcmp`. In a counting loop that is the whole cost: the LangArena
n-gram benchmark spent its time turning 4-grams into strings, not counting
them. This map hashes the integer itself and keeps key and value side by side
in one array, so a lookup touches one cache line and a count increment is one
probe.

```aether,run
import std.intmap

main() {
    votes = intmap.new()

    intmap.put(votes, 7, 1)
    intmap.put(votes, 7, 5)         // overwrite: still one entry
    intmap.put(votes, -3, 40)

    println("size: ${intmap.size(votes)}")
    println("7:    ${intmap.get_or(votes, 7, 0)}")
    println("9:    ${intmap.get_or(votes, 9, 0)}")   // absent: the fallback
    println("has 9: ${intmap.has(votes, 9)}")

    intmap.remove(votes, -3)
    println("after remove: ${intmap.size(votes)}")

    intmap.free(votes)
}
```
```output
size: 2
7:    5
9:    0
has 9: false
after remove: 1
```

Keys and values are full 64-bit `long`s. A value is a bare integer the map
never interprets: store an `int`, a `long`, or a pointer via
`mem.ptr_to_long`. The map frees nothing on your behalf, so a pointer stored
that way is still yours to release.

## Counting with `add`

`add(map, key, delta)` adds to the value under a key, counting an absent key
from 0, and returns the new value. It is one probe, where `has` then `get`
then `put` would be three. Packing a few bytes into one key is how an n-gram
becomes an integer:

```aether,run
import std.intmap
import std.string

main() {
    text = "abracadabra"
    n = string.string_length(text)
    grams = intmap.new()

    i = 0
    while i + 3 <= n {
        // Three bytes packed into one key, most significant first.
        long key = string.string_char_at(text, i) * 65536
        key = key + string.string_char_at(text, i + 1) * 256
        key = key + string.string_char_at(text, i + 2)
        intmap.add(grams, key, 1)
        i = i + 1
    }

    long abr = 97 * 65536 + 98 * 256 + 114
    println("distinct 3-grams: ${intmap.size(grams)}")
    println("abr: ${intmap.get_or(grams, abr, 0)}")

    intmap.free(grams)
}
```
```output
distinct 3-grams: 7
abr: 2
```

## Reading the entries

`get` returns `(value, found)`, so a stored zero and an absent key are
distinguishable. `get_or` is the hot-path form: one probe and no tuple, at
the cost of not telling a stored `fallback` from a missing key.

`keys` and `values` each snapshot the map into a `std.longarr` buffer, which
you release with `longarr.longarr_free`. Order is slot order and unspecified,
but the two snapshots line up index for index as long as the map is not
mutated between the calls. Sort a keys buffer with `sort.longs` for a
deterministic listing.

```aether,run
import std.intmap
import std.longarr
import std.sort

main() {
    m = intmap.new()
    intmap.put(m, 30, 3)
    intmap.put(m, 10, 1)
    intmap.put(m, 20, 2)

    ks, err = intmap.keys(m)
    if err != "" {
        println("keys failed: ${err}")
        return
    }
    sort.longs(longarr.array(ks))
    i = 0
    while i < longarr.longarr_size(ks) {
        k = longarr.longarr_get_raw(ks, i)
        println("${k} -> ${intmap.get_or(m, k, 0)}")
        i = i + 1
    }
    longarr.longarr_free(ks)
    intmap.free(m)
}
```
```output
10 -> 1
20 -> 2
30 -> 3
```

## Failure reporting

`put` returns `false` on a null map or when the table cannot grow. `add`
stores nothing in that case and returns 0, which a legitimate count can also
be, so a caller that must know its data landed checks `has` afterwards.
Calls on a null map are safe: `size` reports 0, `has` reports false, `get_or`
returns its fallback, and `keys` returns `(null, "null map")`.

## Exports

`new`, `put`, `get`, `get_or`, `has`, `add`, `remove`, `size`, `clear`,
`free`, `keys`, `values`, plus the `aether_intmap_*` raw forms.
