# std.sync

An atomic 64-bit integer cell — the small piece of shared mutable state you
can touch from several threads without a lock.

`.ae` already had an atomic *pointer* swap (`std.snapshot`) but no atomic
*integer*, so you could publish a new value but not build the reference count
or the small lock-free retire ring that reclaims the old one. This is that
missing counter: one atomic `i64` with load, store, add, sub, and
compare-and-swap.

```aether,run
import std.sync

main() {
    c = sync.atomic_new(0)

    // add and sub return the NEW value (not the previous one).
    println("add 3: ${sync.atomic_add(c, 3)}")
    println("add 1: ${sync.atomic_add(c, 1)}")
    println("sub 2: ${sync.atomic_sub(c, 2)}")

    // store overwrites; load reads.
    sync.atomic_store(c, 10)
    println("load: ${sync.atomic_load(c)}")

    // cas swaps only if the cell still holds what you expected.
    println("cas 10->20: ${sync.atomic_cas(c, 10, 20)}")
    println("cas 10->30: ${sync.atomic_cas(c, 10, 30)}")

    sync.atomic_free(c)
}
```
```output
add 3: 3
add 1: 4
sub 2: 2
load: 10
cas 10->20: 1
cas 10->30: 0
```

**`add` and `sub` return the value *after* the operation**, unlike C's
`fetch_add`. That is deliberate: the dominant use is a reference count that
must act exactly when it reaches zero, and returning the post-operation value
makes that a plain equality check —

```aether,fragment
// releasing one reference; the last releaser reclaims.
if sync.atomic_sub(refcount, 1) == 0 {
    // every other holder is gone — safe to free the guarded value
}
```

— with no fetch-then-adjust step to fumble across the language boundary.

**Memory ordering is built in.** `load` is an acquire, `store` a release, and
`add`/`sub`/`cas` are both. So when the counter guards other data — a refcount
protecting the value it counts, a flag publishing a freshly-built buffer — a
thread that acquires the counter sees everything the releasing thread wrote
before it. Same publish/subscribe discipline `std.snapshot` uses for its
pointer.

## When to reach for it

This is low-level plumbing for **pool-owned, C-style shared state** — the shape
the reverse-proxy load balancer already uses one layer down in C
(`std/http/proxy/aether_proxy_lb.c`: atomics plus a mutex), lifted into `.ae`.
The motivating case is a `std.snapshot` cell's reclamation: guard each
published value with a refcount, and free it when the count reaches zero after
a grace period, so a pool-owned copy-on-write structure reclaims correctly
instead of leaking its displaced values.

### The retire pattern (why this module exists)

`std.snapshot` publishes an immutable value behind an atomic pointer and hands
the writer back the **displaced** one — but it cannot free that value, because a
reader may have loaded it microseconds before the swap. Without an atomic
integer there was no way to know when the last reader is done, so a pure-`.ae`
snapshot user had to leak every displaced value. A `std.sync` refcount closes
it: each value carries a count, a reader holds a reference across its read, and
the value frees itself the moment the count reaches zero — never while a reader
still holds it.

```aether,run
import std.sync
import std.snapshot
import std.mem

extern malloc(n: int) -> ptr
extern free(p: ptr)

// A guarded value: an int payload + a std.sync refcount. It frees itself
// (payload box AND its count) exactly when the last holder releases.
guarded_new(payload: int) -> ptr {
    g = malloc(16)
    mem.set_int(g, 0, payload)
    rc = sync.atomic_new(1)                    // one ref: the snapshot cell
    mem.set_long(g, 8, mem.ptr_to_long(rc))
    return g
}
guarded_rc(g: ptr) -> ptr { return mem.long_to_ptr(mem.get_long(g, 8)) }
guarded_payload(g: ptr) -> int { return mem.get_int(g, 0) }

// A reader that loaded this value takes a reference.
hold_ref(g: ptr) -> ptr { _n = sync.atomic_add(guarded_rc(g), 1)  return g }

// Drop a reference. At zero, no holder remains — reclaim.
drop_ref(g: ptr) {
    if sync.atomic_sub(guarded_rc(g), 1) == 0 {
        println("  freeing payload ${guarded_payload(g)} (refcount hit 0)")
        sync.atomic_free(guarded_rc(g))
        free(g)
    }
}

main() {
    cell = snapshot.new(guarded_new(100))      // v100, rc=1 (the cell's ref)

    r = hold_ref(snapshot.load(cell))          // a reader loads+holds: v100 rc=2

    // Publish v200; the displaced v100 comes back. The writer drops the CELL's
    // reference — but the reader still holds one, so v100 is NOT freed yet.
    displaced = snapshot.store(cell, guarded_new(200))
    println("published 200, displaced ${guarded_payload(displaced)}")
    drop_ref(displaced)                        // v100 rc=1 (reader still holds)

    drop_ref(r)                                // reader done → v100 frees now

    drop_ref(snapshot.load(cell))              // tear down: v200 rc=0 → frees
    snapshot.free(cell)
    println("done")
}
```
```output
published 200, displaced 100
  freeing payload 100 (refcount hit 0)
  freeing payload 200 (refcount hit 0)
done
```

The displaced `v100` is freed only after **both** the writer's and the reader's
references drop — never at the swap, when a reader still held it. That deferred
free is the whole job, and it needs an atomic integer `std.snapshot` alone does
not provide. (In a real many-thread server the reader runs on a `std.http` pool
thread; the counting is identical, and the free happens on whichever thread
drops the last reference.)

It is **not a mutex and not a general lock**, and not an invitation to replace
the actor model with lock-based sharing — actors remain the default for
coordinating mutable state. Reach for `std.sync` only for the pool-owned,
off-scheduler seam actors do not cover, where a single atomic integer is
exactly enough.

The value is a signed 64-bit integer (Aether `long`), so a refcount, a
generation number, or a pointer punned through `mem.long_to_ptr` all fit. The
cell is memory-cap accounted, like every other stdlib allocation.
