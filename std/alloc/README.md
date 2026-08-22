# std.alloc

An allocator handle, so a subsystem can take "where do I get memory from" as
a parameter instead of hardcoding it.

`alloc.system()` is malloc/free. `alloc.of_arena(ar)` wraps a `std.arena` so
the same code allocates by bump instead. `std.tracking.wrap(inner)` wraps
either and counts what is still live. Because they are all the same handle
type, a caller swaps allocation strategy — or adds leak detection — without
the code under it changing.

`resize` and `release` take the **old size** as well as the pointer. That is
not redundant: an arena and a tracking wrapper both need to know how much they
are reclaiming, and passing the wrong size corrupts their accounting rather
than being ignored.

```aether,run
import std.alloc
import std.mem

main() {
    a = alloc.system()

    p = alloc.raw(a, 32)
    mem.set_int(p, 0, 7)
    println("value: ${mem.get_int(p, 0)}")

    // resize takes (allocator, block, old_size, new_size) and
    // preserves the contents.
    p = alloc.resize(a, p, 32, 128)
    println("after grow: ${mem.get_int(p, 0)}")

    alloc.release(a, p, 128)
    println("released")
}
```
```output
value: 7
after grow: 7
released
```

Swapping `alloc.system()` for `tracking.wrap(alloc.system())` in that example
would leave the body untouched and let the program assert, at the end, that
nothing leaked — see `std/tracking`.

## Exports

`system`, `of_arena`, `arena_free`, `raw`, `resize`, `release`.
