# std.map

A string-keyed map of pointers.

Like `std.list`, it stores `ptr` and does not own the values. Rebinding a key
replaces its value without growing the map, and does not free the old one —
that is the caller's, because the map never claimed it.

`std.spec` keeps its whole framework state in one of these, which is a fair
demonstration of the intended use: a heterogeneous bag of handles keyed by
name.

```aether,run
import std.map
import std.collections

main() {
    m = map.map_new()
    value = collections.list_new()

    err = map.put(m, "key", value)
    println("put err='${err}' size=${map.map_size(m)}")

    println("bound:   ${map.map_has(m, "key")}")
    println("unbound: ${map.map_has(m, "absent")}")

    map.map_free(m)
    collections.list_free(value)
}
```
```output
put err='' size=1
bound:   1
unbound: 0
```

**`get` reports a missing key as success.** It guards `map == null` but not
membership, so `map.get(m, "absent")` returns `(null, "")`. Use `map_has` for
the unambiguous membership test, and null-check the pointer.

`map_keys` returns the key set for iteration, and must be released with
`map_keys_free`. Read it with `keys_size` and `keys_get`.

The returned strings are **borrowed** from the live map: valid until
`keys_free`, and only while the map still holds that entry. The snapshot does
not track later mutation, so a key removed from the map leaves a dangling
entry in a snapshot taken before the removal. Retain or copy anything that has
to outlive either.

Iteration order is bucket order and is **unspecified**. Sort the keys
(`std.sort.strings`) when the output has to be deterministic.

An out-of-range or negative index returns `""` rather than trapping. An empty
string is also a legitimate key, so check `keys_size` first if the difference
matters.

## Exports

`map_new`, `put`, `map_put_raw`, `map_put_string_owned`, `get`,
`map_get_raw`, `map_has`, `map_remove`, `map_size`, `map_clear`, `keys`,
`map_keys_raw`, `map_keys_free`, `keys_size`, `keys_get`, `map_free`.
