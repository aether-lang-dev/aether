# std.snapshot

A copy-on-write cell for read-mostly shared data.

One writer publishes a new immutable value; many readers load the current one
without a lock. That is the trade: readers never block and never see a
half-updated value, and the cost is that the writer builds a whole new version
rather than editing in place. Right for a routing table, a config snapshot, a
feature-flag set — anything read constantly and changed rarely.

```aether,run
import std.snapshot
import std.collections
import std.mem

main() {
    first = collections.list_new()
    cell = snapshot.new(first)

    // load is a lock-free read of the current value.
    got = snapshot.load(cell)
    println("loaded first: ${mem.ptr_to_long(got) == mem.ptr_to_long(first)}")

    // store publishes a new value and RETURNS the displaced one —
    // the writer now owns it and must reclaim it after a grace period.
    second = collections.list_new()
    displaced = snapshot.store(cell, second)
    println("got the old one back: ${mem.ptr_to_long(displaced) == mem.ptr_to_long(first)}")

    // cas swaps only if the cell still holds what you expected.
    println("cas: ${snapshot.cas(cell, second, first)}")

    snapshot.free(cell)
    collections.list_free(first)
    collections.list_free(second)
}
```
```output
loaded first: true
got the old one back: true
cas: 1
```

**`store` returns the displaced value rather than freeing it**, and that is
the part to get right. The cell does not own what it points at, so ignoring
the return leaks the old version. Nor can you free it immediately: a reader
may still be holding it. Reclaim after a grace period — once every reader that
could have loaded it has finished.

`cas` is the read-modify-write form: load, build a successor, swap it in only
if nothing changed underneath. On failure, reload and retry.

`free(cell)` releases the cell, **not** the value inside it.

Every entry point is null-safe: `load(null)` is null, `cas(null, ...)` is 0.

## Exports

`new`, `load`, `store`, `cas`, `free`, plus the `aether_snapshot_*` raw forms.
