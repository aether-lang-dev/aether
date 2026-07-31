# Sharded Actor Map, Lock Striping, the Actor-Native Way

A pattern for a **hot shared map** that has to serve many concurrent
readers and writers without collapsing onto a single core.

> **The short version.** One actor owning a shared map is the "one mutex"
> trap: every `get`/`set` funnels through a single mailbox and serializes,
> so it cannot scale past one core. The fix is the actor counterpart of
> **lock striping**, spawn `N` owner actors, route each key to
> `hash(key) % N`, and operations on different keys land on different
> mailboxes. Contention drops roughly `N×`.

[Design discussion on the issue tracker](https://github.com/aether-lang-dev/aether/issues/839).

See the runnable example at
[`examples/actors/sharded-map.ae`](../../examples/actors/sharded-map.ae) and
the regression test under
[`tests/integration/sharded_actor_map/`](../../tests/integration/sharded_actor_map/).

---

## The single-actor-mailbox trap

The obvious way to "make a map thread-safe with actors" is to wrap it in
one actor:

```aether
actor MapOwner {
    state store = map_new()
    receive {
        Set(key, val) -> { /* ... */ }
        Get(key)      -> { reply GetReply { value: /* ... */ } }
    }
}
```

This is correct, and it is also a trap. An actor processes its mailbox
**one message at a time**. Every caller, no matter how many cores are
running, no matter that they touch completely unrelated keys, lines up
behind the same mailbox and waits its turn. The map owner is a serial
section with extra steps.

This is *exactly* the behaviour of a single global mutex:

- **It serializes all access.** Two cores reading two different keys still
  take turns. The data structure supports concurrency; the single
  guard does not.
- **It negatively scales.** Add cores and throughput does not rise, it
  often *falls*, because the contending cores now also fight over the one
  mailbox's cache line. The line holding the mailbox head/tail
  ping-pongs between cores on every enqueue and dequeue; each transfer is
  a cross-core cache miss. More contenders means more ping-pong, not more
  work done. This is the same cache-line story that makes one mutex a
  scalability ceiling, the actor's mailbox *is* the contended line.
- **Tail latency balloons.** A slow handler (a big rebuild, a `?` ask that
  blocks) stalls every other caller behind it, because they are all in
  the same queue.

The mailbox gives you a clean, race-free programming model. What it does
**not** give you, on its own, is parallelism for a hot shared structure.

---

## The fix: shard by `key.hash % N`

Lock striping splits one lock into an array of `N` locks and assigns each
key to `locks[hash(key) % N]`, so unrelated keys rarely share a lock. The
**actor-native** version replaces locks with owner actors:

1. Spawn `N` **shard actors**, each owning one independent partition of
   the key space (its own private `map`).
2. To operate on `key`, compute `idx = hash(key) % N` and talk to
   `shards[idx]`.
3. `set`/`delete` are fire-and-forget sends (`!`); `get` is a synchronous
   ask (`?`) that the shard answers with `reply`.

Because the partitions are disjoint, two operations on two different keys
that hash to different shards run on **different mailboxes, on different
cores, at the same time**. The serial bottleneck is gone; what remains is
`N` independent small bottlenecks, which is the point.

```aether
// Route, then talk only to the owning shard.
idx = shard_for(key, nshards)   //  hash(key) % nshards
shards[idx] ! Set { key: key, val: v }     // write: fire-and-forget
got = shards[idx] ? Get { key: key }       // read:  synchronous ask
```

This is built **entirely** on Aether's existing ask/reply primitive (the
`?` operator + the `reply` statement); the sharded map adds no runtime
machinery. See [`actor-concurrency.md`](../actor-concurrency.md) for the
ask/reply mechanics and [`examples/actors/ask-pattern.ae`](../../examples/actors/ask-pattern.ae)
for the base pattern.

### Routing hash

Routing needs a fast, well-distributing, **non-cryptographic** hash,
distribution and speed matter, collision-resistance does not. The example
uses **FNV-1a** over the key's bytes, masked to a non-negative 31-bit
value so the `% N` stays in range:

```aether
shard_for(key: string, nshards: int) -> int {
    h = 2166136261                      // FNV offset basis
    n = string.length(key)
    i = 0
    while i < n {
        c = string.char_at(key, i)
        h = h ^ c
        h = h * 16777619                // FNV prime
        h = h & 2147483647              // keep non-negative
        i = i + 1
    }
    return h % nshards
}
```

FNV-1a is a standard, byte-at-a-time hash with good avalanche for short
string keys. Reach for a stdlib digest (`std.cryptography`) only if you
need cryptographic properties, for routing it is pure overhead (it
allocates and returns a hex string, where you want a cheap integer).

---

## How many shards?

This is where the actor version **diverges from the lock-striping
literature**. Striped-lock maps default to a fixed stripe count set
independently of the core count, Java's `ConcurrentHashMap`, for
instance, historically used a default concurrency level of **16**. Do
**not** copy such a number for actors.

A lock stripe is nearly free: a word in an array. An actor shard is not,
each one is a live entity with a **mailbox, a scheduler slot, and a cache
footprint**, and is itself a serialization point. Past a small multiple of
the core count you stop buying parallelism (you only have so many cores to
run shards on) and start paying for idle mailboxes, scheduler overhead, and
worse cache locality.

**Guidance:** size `N` to your hardware, not to a fixed constant.

```
N  ≈  core_count × small_factor          // small_factor ∈ ~1..4
```

- Start at roughly **core count** (`1×`). This gives one busy shard per
  core in the steady state.
- Nudge up to **2×–4×** if handlers are short and you see imbalance, extra
  shards smooth out uneven load and reduce the odds that two hot keys share
  a shard.
- Going much past that is counter-productive: more mailboxes than cores
  just adds scheduling and cache cost for no extra parallelism.

The example uses `N = 4` for readability; in production derive `N` from the
runtime core count.

---

## The skew caveat (hot keys)

Sharding assumes keys are **roughly uniformly accessed**. Real workloads
often are not, they are **Zipfian**: a handful of keys take the lion's
share of traffic. Those hot keys hash to a few specific shards, and *those*
shards become serial bottlenecks again, exactly like the single-actor case
just at `1/N` scale instead of `1/1`. Adding shards does **not** help: a
single hot key cannot be split across shards, because all of its
operations must reach the one actor that owns it.

If your access pattern is **read-mostly and hot-key-skewed** (a config
blob, a routing table, feature flags hammered on every request),
**sharding is the wrong tool**, point those reads at the **copy-on-write
snapshot cell** instead: [`std.snapshot` / `docs/design/snapshot-cell.md`](snapshot-cell.md)
(see the [snapshot-cell design](snapshot-cell.md)). There a
read is a single lock-free atomic load, so a thousand readers of the same
hot key never contend at all. Sharding is for **write-heavy, well-spread**
key spaces; the snapshot cell is for **read-heavy** ones.

---

## What this is *not*

This is deliberately **not** a `sync.Map`-style drop-in: there is no shared
object you call `.get()`/`.set()` on from many threads with the runtime
hiding the synchronization. That API shape runs *against* the actor grain,
it reintroduces shared mutable state and the very contention the actor
model exists to remove. The sharded actor map keeps the grain intact:
**state lives inside actors, and you reach it by sending messages.** The
sharding is just *which* actor you send to.

---

## Operation routing at a glance

| Op       | Transport        | Routes to                  | Returns                         |
| -------- | ---------------- | -------------------------- | ------------------------------- |
| `set`    | `!` (send)       | `shards[hash(key) % N]`    | nothing (fire-and-forget)       |
| `delete` | `!` (send)       | `shards[hash(key) % N]`    | nothing (fire-and-forget)       |
| `get`    | `?` (ask)        | `shards[hash(key) % N]`    | the value (via `reply`)         |

Note the ask returns the **first field** of the reply message, so a `get`
reply puts the value first (`message GetReply { value: int }`). A natural
idiom for "absent" is a sentinel in that field (the example uses `-1`).

### A note on the `get` handler

Compute the result into a local and issue **one** `reply` at the end of the
handler:

```aether
Get(key) -> {
    result = 0 - 1                  // ABSENT
    v = map_get_raw(store, key)
    if v != null {
        n, _ = string.to_int(v)
        result = n
    }
    reply GetReply { value: result }   // single, trailing reply
}
```

A single trailing `reply` is the robust shape for an ask handler and keeps
the value-extraction unambiguous. Build the answer first, reply once.

---

## What to actually use (and a benchmark)

The three designs Aether ships for a concurrently-accessed map line up as
follows:

| Access pattern | Use | Why |
|----------------|-----|-----|
| General concurrent map (mixed read/write, well-spread keys) | **Sharded actor map** (this page) | per-key routing across `N` mailboxes; the general winner |
| Read-mostly (config, routing tables, flags) | **COW snapshot cell** ([`std.snapshot`](snapshot-cell.md)) | lock-free reads; each write rebuilds the whole value |
| Genuinely low contention | Single owner-actor | the simplest correct thing, until contention rises |

A runnable scaling study that measures all three under read-only,
read-heavy, balanced, and write-heavy mixes lives at
[`benchmarks/concurrent-cache/concurrent_cache_bench.ae`](../../benchmarks/concurrent-cache/concurrent_cache_bench.ae);
see [`docs/design/concurrent-cache-benchmark.md`](concurrent-cache-benchmark.md)
for how to run it and how to read the ops/sec it prints.
