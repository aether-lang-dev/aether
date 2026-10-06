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

**`get` checks the index.** `list.get(l, 99)` on a shorter list returns
`(null, "index out of range")`, and a null list returns `(null, "null list")`,
so an empty error always means the value is good. `list_get_raw` is the
unchecked form, for a loop that has already bounded the index by
`list_size`.

## Exports

`list_new`, `list_new_in`, `add`, `list_add_raw`, `list_add_string_owned`,
`get`, `list_get_raw`, `list_set`, `list_size`, `list_remove`, `list_clear`,
`list_free`.
