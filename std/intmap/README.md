# std.intmap

A hash map from `long` keys to `long` values.

`std.map` keys by string, so a program counting integer keys (n-grams packed
into a long, ids, grid coordinates) rendered every key with
`string.from_long` and paid an allocation and a string hash on each lookup.
LangArena's Distance::NGram ran at 26.6x Go on exactly that (#1986).
`std.intmap` keys by the integer itself.

```aether,run
import std.intmap

main() {
    counts = intmap.new(0)
    words = [3, 7, 3, 3, 9, 7]
    for i in 0..6 {
        intmap.add(counts, words[i], 1)     // one probe per count
    }
    println("3 seen ${intmap.get(counts, 3, 0)} times")
    println("${intmap.size(counts)} distinct")
    println("4 seen ${intmap.get(counts, 4, 0)} times")
    intmap.free(counts)
}
```
```output
3 seen 3 times
3 distinct
4 seen 0 times
```

Counting two million packed 4-grams of a pseudo-random letter stream (57,122
distinct), best of three on an i7-1265U, Windows/UCRT64:

| | time |
|---|---|
| `std.map` keyed by `string.from_long` | 256 ms |
| `std.intmap.add` | 16 ms |

## How it works

Open addressing with linear probing over a power-of-two table. Keys are
spread by the splitmix64 finalizer first, so sequential and strided keys
(`i * 1024`) do not cluster into long probe runs. A removed key leaves a
tombstone; when live keys plus tombstones pass 70% of the table it is rebuilt,
doubled when the live keys alone pass half. Storage goes through the
resource-cap allocator like the other collections.

Values are `long`. A pointer goes in through std.mem's `ptr_to_long` and
comes out through `long_to_ptr`; the map owns nothing a value points at.

## Iteration

In no particular order, by slot: `next(m, slot)` is the first occupied slot at
or after `slot`, or `-1`.

```aether,fragment
s = intmap.next(m, 0)
while s >= 0 {
    println("${intmap.key_at(m, s)} = ${intmap.value_at(m, s)}")
    s = intmap.next(m, s + 1)
}
```

A `put` or `add` of a new key, or `clear`, may rebuild the table and move
every slot: finish the walk (or collect the keys) before changing the map.

## API

| Call | Returns | Notes |
|---|---|---|
| `new(capacity_hint)` | `ptr` | room for about `capacity_hint` keys before growing; `0` for the default; null on allocation failure |
| `free(m)` | | |
| `put(m, key, value)` | `int` | `1` new key, `0` replaced, `-1` allocation failure or null map |
| `get(m, key, missing)` | `long` | `missing` when the key is absent |
| `has(m, key)` | `bool` | |
| `add(m, key, delta)` | `long` | the new value; an absent key starts at `0`; allocation failure panics, as `make` does |
| `remove(m, key)` | `bool` | `true` when it was present |
| `size(m)` | `int` | |
| `clear(m)` | | keeps the table's capacity |
| `next(m, slot)` | `int` | the next occupied slot, or `-1` |
| `key_at(m, slot)` / `value_at(m, slot)` | `long` | |

A null map reads as empty: `get` returns `missing`, `size` is `0`, `put`
returns `-1`.
