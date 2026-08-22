# std.pqueue

A priority queue over `(priority, item)` pairs, backed by a binary heap.

Push and pop are O(log n); peek and size are O(1). **The lowest priority
value comes out first** — it is a min-heap. For largest-first, negate the
priority on the way in.

Items are stored as `ptr`, so the queue holds whatever the caller points at
and does not own it.

```aether,run
import std.pqueue
import std.mem

main() {
    // Three items, tagged by pointer value so the ordering is visible.
    q = pqueue.new()
    pqueue.push(q, 5, mem.long_to_ptr(100))
    pqueue.push(q, 1, mem.long_to_ptr(200))
    pqueue.push(q, 3, mem.long_to_ptr(300))

    println("size: ${pqueue.size(q)}")

    // Priority 1 first, then 3 — not insertion order.
    println("first:  ${mem.ptr_to_long(pqueue.pop(q))}")
    println("second: ${mem.ptr_to_long(pqueue.pop(q))}")
    println("left:   ${pqueue.size(q)}")

    pqueue.free(q)
}
```
```output
size: 3
first:  200
second: 300
left:   1
```

Equal priorities have no defined order relative to each other — a binary heap
is not stable. Where insertion order must break ties, fold a monotonic counter
into the priority.

## Exports

`new`, `push`, `pop`, `peek`, `size`, `is_empty`, `clear`, `free`.
