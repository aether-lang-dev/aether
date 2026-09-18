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

It is **not a mutex and not a general lock**, and not an invitation to replace
the actor model with lock-based sharing — actors remain the default for
coordinating mutable state. Reach for `std.sync` only for the pool-owned,
off-scheduler seam actors do not cover, where a single atomic integer is
exactly enough.

The value is a signed 64-bit integer (Aether `long`), so a refcount, a
generation number, or a pointer punned through `mem.long_to_ptr` all fit. The
cell is memory-cap accounted, like every other stdlib allocation.
