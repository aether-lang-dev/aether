# std.list

A growable list of pointers.

The list stores `ptr` and does not own what they point at: freeing the list
frees its own storage, not the elements. That is what lets the same container
hold borrowed references, arena-allocated values, or handles from another
module without a lifetime argument.

`add` and `get` are the `(value, err)` wrappers; the `list_*` forms are the
direct ones.

```aether,run
import std.list
import std.collections
import std.mem

main() {
    l = list.list_new()

    // Any pointer will do; a second list makes a convenient one.
    item = collections.list_new()

    err = list.add(l, item)
    println("add err='${err}' size=${list.list_size(l)}")

    got, gerr = list.get(l, 0)
    println("same pointer back: ${mem.ptr_to_long(got) == mem.ptr_to_long(item)}")

    list.list_free(l)
    collections.list_free(item)
}
```
```output
add err='' size=1
same pointer back: true
```

**`get` reports an out-of-range index as success.** It guards `list == null`
but not the index, so `list.get(l, 99)` returns `(null, "")` — a caller
following the `(value, err)` convention reads that as a hit. Null-check the
returned pointer as well as the error until that changes.

## Exports

`list_new`, `list_new_in`, `add`, `list_add_raw`, `list_add_string_owned`,
`get`, `list_get_raw`, `list_set`, `list_size`, `list_remove`, `list_clear`,
`list_free`.
