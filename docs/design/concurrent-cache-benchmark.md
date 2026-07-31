# Concurrent Cache Designs, Benchmark & Guidance

A runnable scaling study comparing the **three** concurrency designs
Aether ships for a shared, concurrently-accessed map, and the guidance
that falls out of it.

[Design discussion on the issue tracker](https://github.com/aether-lang-dev/aether/issues/841).
Benchmark:
[`benchmarks/concurrent-cache/concurrent_cache_bench.ae`](../../benchmarks/concurrent-cache/concurrent_cache_bench.ae).

---

## What it measures

The benchmark builds the same string → int map three different ways and
drives an **identical, deterministic operation stream** through each one
under four read/write mixes, timing each design × mix with the monotonic
clock (`os.now_monotonic_ns`) and printing **ops/sec**.

| # | Design | Read path | Write path | Source |
|---|--------|-----------|------------|--------|
| 1 | **Single-owner actor** | `?` ask to the one owner | `!` to the one owner | the "one mutex" trap (`docs/design/sharded-actor-map.md`) |
| 2 | **Sharded actor map** | `?` ask to `shards[hash(key)%N]` | `!` to `shards[hash(key)%N]` | `examples/actors/sharded-map.ae` |
| 3 | **COW snapshot cell** | lock-free `snapshot.load`, no mailbox | rebuild the whole map, `snapshot.store`, defer-free the displaced snapshot by one generation | `std.snapshot` / `docs/design/snapshot-cell.md` |

**Workload mixes** (each runs a fixed 100 000-op budget over 64 keys):

- **read-only**, 100 % reads
- **read-heavy**, 90 % reads / 10 % writes
- **balanced**, 50 / 50
- **write-heavy**, 10 % reads / 90 % writes

A single 31-bit LCG produces the read/write split and key selection, and
every design replays the **same** stream for a given mix, so the only
variable across the three numbers in a row is the data structure itself.

All operations are issued from the single `main` loop; there are no
separate reader actors. Reads exercise each design's read path: an `?`
ask to the one owner (design 1), an `?` ask routed to `shards[hash(key)%N]`
(design 2), or a direct lock-free `snapshot.load` (design 3). Writes go
to the single owner (design 1), the routed shard (design 2), or, for COW,
rebuild the whole map inline and republish it with `snapshot.store`.
Fanning reads out across concurrent driver actors to expose the sharded
read path's parallelism is a documented out-of-scope refinement (see
"Honest limitations" below).

### Run it

```bash
AETHER_HOME=. ae run benchmarks/concurrent-cache/concurrent_cache_bench.ae
```

It prints a table per mix and ends with a `PASS concurrent_cache_bench`
marker for smoke testing. **The ops/sec are measured live on your
machine, nothing in the program or this document is a hardcoded
number.** Paste your run's table into a PR or issue when you cite it.

---

## What to actually use

This is the practical takeaway, the same conclusion the Go
"concurrent cache designs" study reaches, mapped onto Aether's
primitives.

| If your access pattern is… | Use | Why |
|----------------------------|-----|-----|
| **General concurrent map** (mixed read/write, keys well spread) | **Sharded actor map** | Per-key routing spreads load across `N` mailboxes; unrelated keys run in parallel. The general-purpose winner. |
| **Read-mostly** (config, routing tables, feature flags; writes rare) | **COW snapshot cell** (`std.snapshot`) | Reads are lock-free atomic loads, a thousand readers of a hot key never contend. Each write rebuilds the whole value, so this only pays off when reads vastly outnumber writes. |
| **Genuinely low contention** (cold map, few callers, simplicity first) | **Single owner-actor** | One mailbox is the simplest correct thing. Fine until contention rises, then it becomes the bottleneck. |

The trap to avoid: a **single owner-actor for a hot map**. It is correct
but it serializes every operation through one mailbox and **negatively
scales**, adding cores makes it slower, because the contending cores now
also fight over that one mailbox's cache line. That is the actor-native
shape of the single-global-mutex anti-pattern.

---

## Expected shape of the results

These are the *qualitative* relationships the design predicts (and that
the Go study found); the benchmark prints the concrete numbers so you can
confirm them on your hardware rather than trusting a table here:

- **Single-owner actor** is the floor on the read-heavy mixes. Every read
  is a synchronous `?` ask that serializes behind one mailbox, so its
  throughput is roughly flat across mixes and does not improve with more
  reader pressure, it is the single-core ceiling.
- **Sharded actor map** is the general winner across the mixed workloads.
  With keys spread across 8 shards, reads and writes on different keys
  proceed on different mailboxes concurrently. It should clearly beat the
  single owner on every mix that has real key spread.
- **COW snapshot cell** should lead the read-only and read-heavy mixes,
  reads are bare lock-free loads with no mailbox arbitration over the
  data, and then fall off as the write fraction climbs, because every
  write rebuilds the entire 64-key map and republishes it (an O(size)
  cost the read-mostly design deliberately trades for cheap reads). On
  the write-heavy mix it is expected to be the slowest of the three.

If your measured table contradicts this shape, that itself is the useful
signal, e.g. on a single-core CI box the sharded design cannot show its
parallelism, and the numbers will compress toward each other.

---

## Honest limitations (and the documented refinement)

This benchmark is a **single, non-core-pinned run**. Treat its numbers as
a directional smoke test, not a publication-grade measurement. The known
gaps, and the refinement that would close them:

1. **No core pinning / no 1..N core sweep.** The Go study's headline
   result, that the single lock *negatively* scales, only shows up when
   you pin worker threads to cores and sweep the core count from 1 to N,
   plotting throughput against cores. Aether's scheduler places actors on
   its worker pool without per-actor core affinity here, and the driver
   issues operations from one `main` loop, so we measure aggregate
   throughput at one operating point, not a scaling curve. **Refinement:**
   pin the scheduler's worker threads to cores, parameterize the number
   of concurrent driver actors, and run the matrix for cores ∈ {1,2,4,8},
   emitting a throughput-vs-cores curve per design.

2. **Uniform key distribution.** Keys are selected uniformly at random,
   which is the *best* case for sharding. Real workloads are usually
   **Zipfian**, a few hot keys dominate, and that is exactly where
   sharding degrades (hot keys collide on a few shards) and the COW cell
   shines (a hot read key is just a repeated lock-free load). **Refinement:**
   add a Zipfian key generator (skew parameter `s ≈ 0.99`) and report a
   second table under skew. See the "skew caveat" in
   [`docs/design/sharded-actor-map.md`](sharded-actor-map.md).

3. **Single run, no warmup statistics.** One timed pass per design × mix,
   no median-of-N or coefficient-of-variation. The
   [cross-language runner](../../benchmarks/cross-language/run_benchmarks.ae)
   shows the median-of-N + CV% pattern to adopt here. **Refinement:**
   wrap each timed loop in a runs/warmup loop and report median + spread.

None of these change the *guidance*, they sharpen the *numbers*. The
relationships in "What to actually use" are structural properties of the
three designs, not artifacts of one machine.

---

## See also

- [`docs/design/sharded-actor-map.md`](sharded-actor-map.md), the sharded
  design, shard-count guidance, and the Zipfian skew caveat.
- [`docs/design/snapshot-cell.md`](snapshot-cell.md), the COW cell, the
  reclamation contract, and when read-mostly is the right call.
- [`examples/actors/sharded-map.ae`](../../examples/actors/sharded-map.ae),
  the canonical sharded-map example the benchmark's design 2 mirrors.
