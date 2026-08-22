# std.collections

The container primitives, in one module: pointer lists, string-keyed maps,
owned-string vectors and packed int arrays.

Most of this surface is also available as focused modules — `std.list`,
`std.map`, `std.intarr` — which is usually the clearer import. Reach for
`std.collections` when a file needs several of them and one import reads
better than four.

The two families it uniquely carries are `string_list` (a vector that owns its
strings, with a lexicographic sort) and the `_in` variants that allocate from
a caller-supplied allocator rather than the system one.

```aether,run
import std.collections

main() {
    names = collections.string_list_new()
    collections.string_list_add(names, "b")
    collections.string_list_add(names, "a")

    // sort_lex orders by byte value, so an uppercase initial would
    // sort before every lowercase one.
    collections.string_list_sort_lex(names)
    println("first: ${collections.string_list_get(names, 0)}")
    println("size:  ${collections.string_list_size(names)}")

    collections.string_list_free(names)
}
```
```output
first: a
size:  2
```

`list_new_in(allocator)` and `map_new_in(allocator)` take a `std.alloc` handle,
so a request-scoped container can be carved from an arena and freed in one
`arena.reset` rather than element by element.

## Exports

`list_new`, `list_new_in`, `list_add`, `list_add_raw`, `list_get`,
`list_get_raw`, `list_set`, `list_size`, `list_remove`, `list_clear`,
`list_free`; `map_new`, `map_put`, `map_put_raw`, `map_get`, `map_get_raw`,
`map_has`, `map_remove`, `map_size`, `map_clear`, `map_keys`, `map_keys_raw`,
`map_keys_free`, `map_free`; `string_list_new`, `string_list_add`,
`string_list_get`, `string_list_set`, `string_list_size`,
`string_list_remove`, `string_list_clear`, `string_list_sort`,
`string_list_sort_lex`, `string_list_free`; and the `intarr_*` forms.
