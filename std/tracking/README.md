# std.tracking

A leak-detecting wrapper around another allocator.

`wrap(inner)` returns an allocator that forwards every request to `inner` and
counts what is still live. Because it is the same handle type as
`alloc.system()` or `alloc.of_arena(...)`, dropping it in changes nothing
about the code being measured — which is the point: the thing you test should
be the thing you ship.

```aether,run
import std.tracking
import std.alloc

main() {
    t = tracking.wrap(alloc.system())

    a = alloc.raw(t, 64)
    b = alloc.raw(t, 32)
    println("live: ${tracking.count(t)} blocks, ${tracking.bytes(t)} bytes")

    // release takes the size, so the byte total falls by the right
    // amount rather than merely the count falling by one.
    alloc.release(t, a, 64)
    println("after one free: ${tracking.count(t)} blocks, ${tracking.bytes(t)} bytes")

    alloc.release(t, b, 32)
    println("balanced: ${tracking.count(t)} blocks")

    tracking.destroy(t)
}
```
```output
live: 2 blocks, 96 bytes
after one free: 1 blocks, 32 bytes
balanced: 0 blocks
```

`report(t)` returns the number of live blocks **and prints them to stdout** —
use it at the end of a run or a test to turn a leak into a visible failure.
It is not silent, so keep it out of a block whose output you are asserting.

## Exports

`wrap`, `count`, `bytes`, `report`, `destroy`.
