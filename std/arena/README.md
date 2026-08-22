# std.arena

A bump allocator: carve many small allocations out of one block, then free
them all at once.

There is no per-allocation free. `reset` rewinds the whole arena to empty in
constant time, and `destroy` returns the block to the system. That trade —
give up individual frees, get allocation down to a pointer bump and
deallocation down to nothing — is what makes an arena right for
request-scoped or frame-scoped work, where everything dies together anyway.

```aether,run
import std.arena
import std.mem

main() {
    ar = arena.create(4096)

    p = arena.alloc(ar, 64)
    mem.set_int(p, 0, 42)

    println("used: ${arena.get_used(ar)} of ${arena.get_size(ar)}")
    println("value: ${mem.get_int(p, 0)}")

    // reset frees everything at once. Every pointer handed out
    // before this line is now dangling — that is the deal.
    arena.reset(ar)
    println("after reset: ${arena.get_used(ar)}")

    arena.destroy(ar)
}
```
```output
used: 64 of 4096
value: 42
after reset: 0
```

`alloc_aligned(ar, size, align)` is the form to reach for when the memory
will be read through `std.mem`'s wider accessors: `set_long` and the u64
pair at an unaligned offset are undefined on strict-alignment targets even
where x86 tolerates them.

`std.alloc` can wrap an arena as a general allocator (`alloc.of_arena`), which
is how a subsystem takes an allocator parameter without caring that the memory
behind it is arena-backed.

## Exports

`create`, `alloc`, `alloc_aligned`, `reset`, `destroy`, `get_used`,
`get_size`, plus the `arena_*` raw forms.
