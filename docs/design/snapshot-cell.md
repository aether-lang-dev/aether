# `std.snapshot` Copy-on-Write Snapshot Cell

A **copy-on-write snapshot cell** for read-mostly shared data: configuration,
routing tables, feature flags, anything read on nearly every request but
rebuilt only rarely.

> **Reads are lock-free.** A read is a single atomic acquire load of a
> pointer. Readers never block, never spin, never coordinate.

Import with:

```aether
import std.snapshot
```

[Design discussion on the issue tracker](https://github.com/aether-lang-org/aether/issues/840).

---

## The idea

The cell holds **one atomic pointer** to an immutable value (the "snapshot").
Many actors read the current snapshot concurrently. A single writer
occasionally builds a brand-new snapshot and publishes it by swapping the
pointer. Because a read is just an acquire load of that pointer, readers touch
no shared lock and contend nothing but the pointer's cache line, and only when
it actually changes.

This is the read-mostly answer. It is the structure behind Go's
`atomic.Pointer`, Linux's RCU, and Java's `CopyOnWriteArrayList`. Aether
differs from all three in one decisive way: **Aether has no garbage collector**,
so reclaiming the displaced snapshot is the caller's job and must be done with
care. See [Reclamation contract](#reclamation-contract-read-this), it is the
whole point of this page.

---

## API

| Call | Signature | Meaning |
|------|-----------|---------|
| `snapshot.new` | `(initial: ptr) -> ptr` | Allocate a cell pointing at `initial` (may be `null`). Returns the cell, or `null` on alloc failure / memory-cap exceeded. Does **not** take ownership of `initial`. |
| `snapshot.load` | `(cell: ptr) -> ptr` | **Lock-free** read: atomic acquire load of the current snapshot. `null` cell → `null`. |
| `snapshot.store` | `(cell: ptr, new_value: ptr) -> ptr` | Atomically publish `new_value` (release exchange) and **return the displaced (old) snapshot** for the writer to reclaim. `null` cell → `null`. |
| `snapshot.cas` | `(cell: ptr, expected: ptr, new_value: ptr) -> int` | Compare-and-swap. If the cell holds `expected`, swap in `new_value` and return `1`; else return `0` and leave it unchanged. `null` cell → `0`. |
| `snapshot.free` | `(cell: ptr)` | Free the **cell itself**. Does **not** free the snapshot it points at. `null`-safe. |

The cell struct is allocated through `aether_caps_malloc` and freed through
`aether_caps_free`, so it participates in the process-wide memory cap
(`--emit=lib` resource caps). The snapshot **values** are caller-owned and
accounted by whoever allocates them.

### Memory ordering

`load` is an **acquire** load; `store`/`cas` publish with **release**. This
pairing is mandatory, not a tuning knob: it guarantees that a reader which
observes a freshly-published pointer also observes every write the writer made
while building that snapshot. Relaxed ordering would let a reader see the new
pointer but a stale, half-built value, a real data race.

---

## Reclamation contract (read this)

**The cell never owns or frees the snapshot values.** It stores only the
pointer. `store` and `cas` succeed by swapping the pointer and **returning the
displaced (old) snapshot** so the **writer** can reclaim it.

But here is the trap. The displaced snapshot may still be in use by an
in-flight reader that called `load` microseconds before the swap:

```
reader:  p = load(cell)          // got snapshot N
writer:                  store(cell, N+1) -> returns N
reader:  ...still reading p (N)...
writer:  free(N)                 // USE-AFTER-FREE, reader still holds N
```

So the writer must **defer** the free until a **grace period** has elapsed,
an interval during which every reader that could be holding the old pointer has
finished its read. This is exactly the RCU `synchronize` / `call_rcu`
discipline. It is **not** a workaround; it is the correct, standard approach for
reclaiming lock-free-read data in a language without a garbage collector.

> **Auto-freeing the displaced snapshot inside `store` would be a
> use-after-free.** `std.snapshot` deliberately does not do it. Returning the
> old pointer and making reclamation explicit is the honest design.

### The actor-writer pattern (defer one generation)

In Aether's actor model the grace period falls out naturally. Route all writes
through a single **writer-actor**. Readers are other actors that `load` and
finish handling one message before processing the next. Because an actor reads
the snapshot, uses it, and returns, all within one message handler, a reader
cannot hold a pointer across two of the writer's publishes. So the writer can
safely **defer one generation**: when it publishes snapshot N+1, it frees
snapshot N−1.

```aether
import std.snapshot

// Writer-actor: owns the cell plus the one deferred (previous) snapshot.
// `cell` holds the live snapshot; `prev` is snapshot N-1, awaiting its
// grace period.
actor ConfigWriter {
    state cell = null    // set to snapshot.new(initial) at startup
    state prev = null    // snapshot N-1, awaiting its grace period

    receive {
        // `next` is the freshly-built snapshot N+1.
        Publish(next) -> {
            // Publish N+1. `displaced` is snapshot N.
            displaced = snapshot.store(cell, next)

            // By now every reader that could hold N-1 (`prev`) has long
            // since finished, each processed at least one message
            // between the publish of N-1 and now. Reclaim it.
            if prev != null {
                free_config(prev)
            }

            // Defer the just-displaced snapshot one more generation.
            prev = displaced
        }
    }
}
```

Tune the deferral depth to your reader latency: if a reader might hold a pointer
across *k* publishes, defer *k* generations (a small ring of pending frees). One
generation is enough whenever a read completes within a single message handler,
which is the common case.

### The CAS loop (read-modify-write)

Use `cas` when the new snapshot is derived from the current one and concurrent
writers are possible (or just for a uniform pattern):

```aether
loop {
    cur  = snapshot.load(cell)
    next = build_next_from(cur)        // pure; allocates `next`
    if snapshot.cas(cell, cur, next) == 1 {
        // Won. `cur` is now displaced, reclaim it after a grace
        // period (defer it), NOT immediately.
        defer_reclaim(cur)
        break
    }
    // Lost the race: another writer published first. Free our unused
    // candidate and retry against the new current value.
    free_value(next)
}
```

On **success** the caller owns the displaced `cur` (reclaim after a grace
period). On **failure** nothing was published, so the caller still owns `next`
and frees it before retrying.

---

## When to use it, and when not to

**Use `std.snapshot` for read-mostly data:**

- Configuration reloaded on SIGHUP or a control-plane push.
- Routing / dispatch tables rebuilt when topology changes.
- Feature-flag sets refreshed every few seconds.
- Any value where reads vastly outnumber writes and each write rebuilds the
  whole value.

**Do not use it for:**

- **Write-heavy data.** Every publish rebuilds the entire value, so a write
  costs O(value size). If writes are frequent, that copy cost dominates.
- **Large values that change often.** Same reason, the per-write rebuild is
  the cost you are trading reads against.
- **Fine-grained mutation.** This cell swaps a whole value atomically; it is not
  a concurrent map with per-key updates. For a write-heavy, well-spread key
  space, use the [sharded actor map](sharded-actor-map.md) instead, it
  partitions per-key writes across `N` owner actors.

### Why not `RWMutex`?

A read/write mutex is the trap this primitive replaces. A read lock still writes
a shared cache line on every acquire/release, so readers contend with each other
even though they only read; under heavy read load that line ping-pongs across
cores. And a write lock starves readers for the duration of the rebuild. The COW
cell's read path touches nothing shared but the snapshot pointer, and only when
it actually changes, so N readers cost nothing extra beyond N pointer loads,
and a publish never blocks a reader. For read-mostly data the COW cell is the
better answer.

---

## See also

- [`docs/memory-management.md`](../memory-management.md), Aether's
  deterministic, no-GC memory model and `defer`.
- [`docs/actor-concurrency.md`](../actor-concurrency.md), the actor model that
  makes the one-generation grace period natural.
- [`docs/design/sharded-actor-map.md`](sharded-actor-map.md), the complementary
  pattern for **write-heavy, well-spread** key spaces (this cell is for
  read-mostly data).
