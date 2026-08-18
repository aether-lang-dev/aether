# Allocators

Aether's memory model is deterministic scope-exit cleanup with no garbage
collector. Most code never thinks about an allocator: values are freed when
they leave scope. `std.alloc` adds an explicit, swappable allocator for the
cases that want to control *where* a data structure's memory comes from, a
request-scoped arena, a fixed pool, or a leak-tracking wrapper in tests.

## The convention (`std.alloc`)

An allocator is a plain handle (`ptr`), not an implicit ambient context. You
pass it explicitly to the entry points that accept one. This keeps data flow
visible and composes with the actor boundary: an actor can hold its own
allocator in `state` without any hidden per-call parameter.

```aether,fragment
import std.alloc

a  = alloc.system()          // malloc/free; the default everywhere
p  = alloc.raw(a, 128)       // 128 bytes
p  = alloc.resize(a, p, 128, 256)
alloc.release(a, p, 256)
```

| Function | Meaning |
|---|---|
| `alloc.system()` | The process-wide system allocator (malloc/realloc/free). Static, never freed. |
| `alloc.raw(a, n)` | Allocate `n` bytes through `a`. Null on failure. |
| `alloc.resize(a, p, old, n)` | Resize `p` from `old` to `n` bytes. `n == 0` frees and returns null; a null `p` allocates. |
| `alloc.release(a, p, n)` | Release a block. `n` is the size it was allocated with (a hint size-aware allocators use). |

### Arena-backed

`alloc.of_arena` turns an existing `std.arena` into an allocator: every
allocation bump-allocates from the arena and individual frees are no-ops.
Reclaim everything at once with `arena.reset` (O(1)); it is ideal for a
request-scoped structure that is built up and thrown away as a unit.

```aether,fragment
import std.alloc
import std.arena

ar = arena.create(64 * 1024)
a  = alloc.of_arena(ar)
// ... build a big request-scoped structure through `a` ...
arena.reset(ar)              // frees everything the request allocated, at once
alloc.arena_free(a)          // frees the handle (not the arena)
arena.destroy(ar)
```

## Collections that allocate through an allocator

Collection constructors gain an `_in` variant that takes an allocator; the
default constructor is unchanged and uses the system path. The collection's
own memory (its control struct and backing arrays) is then owned by the
allocator, so an arena or a tracking wrapper manages the container. Element
ownership is unchanged.

```aether,fragment
import std.list (list_new_in, add)
import std.arena
import std.alloc (of_arena)

ar = arena.create(1024 * 1024)
l  = list_new_in(of_arena(ar))   // the list lives in the arena
add(l, item)
// ...
arena.reset(ar)                  // frees the list and its array
```

`std.list` (`list_new_in`) is the first collection to adopt this. Others
(`map`, `bytes`, `strbuilder`) follow the same shape.

## Leak detection in tests (`std.tracking`)

Because there is no GC backstop, a missed `release`/`free` is a silent leak.
`std.tracking` wraps any allocator, records every live allocation, and reports
the outstanding set, turning a leak from something only a coarse external CI
gate can catch into an ordinary in-test assertion.

```aether,fragment
import std.alloc
import std.tracking

t = tracking.wrap(alloc.system())

l = list_new_in(t)            // or alloc.raw(t, n), or any _in constructor
// ... exercise the code under test ...

if tracking.count(t) != 0 {   // 0 == no leaks
    tracking.report(t)        // prints each outstanding block (order + size)
}
tracking.destroy(t)
```

| Function | Meaning |
|---|---|
| `tracking.wrap(inner)` | Wrap `inner` (e.g. `alloc.system()`); returns an allocator handle. |
| `tracking.count(t)` | Live (allocated but not freed) block count. |
| `tracking.bytes(t)` | Total live bytes. |
| `tracking.report(t)` | Print each outstanding block to stderr; returns the live count. |
| `tracking.destroy(t)` | Free the tracker. Does not free the leaked blocks (those are the leaks). |

The tracker allocates its own bookkeeping with the system allocator, so it
never counts itself. `tracking.destroy` deliberately does not free the
still-live blocks: they are exactly the leaks the code under test failed to
release, and freeing them would hide the bug.
