<!-- Cross-reference: Bend/HVM vs. Aether, and the fork-join measurement that settles #1297. -->
<!-- Kept next to the code so the numbers stay discoverable rather than living in an issue thread. -->

# Fork-join over `std.worker`: the measurement (#1297)

> **Status:** Measured, August 2026, and the answer is recorded here rather than
> in a roadmap. The idea came from [Bend/HVM](https://higherorderco.com), whose
> *engine* Aether deliberately does not adopt (an interaction-combinator graph
> reducer is the VM-with-GC-adjacent-semantics runtime the no-VM/no-GC identity
> rejects). What was worth taking is the ergonomic idea: parallelism expressed
> as structure rather than as thread code.

## The premise was half stale

#1297 opens with "Missing: any `parallel_map` / parallel fold combinator."
That was true when it was filed and is no longer:
[`worker.map(items, f)`](../stdlib-reference.md) ships today, bounded by the
existing pool and index-aligned with its input.

So the open question was never "should we build a parallel map", it was "does
the layer that exists earn its place, and what is still missing". The issue's
own answer to that is the benchmarks, which is what this note records.

Still genuinely missing: a parallel **fold/reduce**.

## Measurements

Machine: macOS arm64, 8 cores. Kernel is a branch-light, allocation-free
integer loop, so the numbers measure the fan-out layer and not the allocator.
Harness in [`benchmarks/fork-join/`](../../benchmarks/fork-join/).

A global sink consumes each result. Without one the optimiser deletes the whole
loop, and the first run of this benchmark duly reported 0 us of sequential
work; any future edit to these files needs to keep the sink.

### 1. Speedup vs fan-out (760 us of work per item)

| items | sequential | parallel | speedup |
|---:|---:|---:|---:|
| 1 | 760 us | 829 us | 0.91x |
| 2 | 1431 us | 740 us | 1.93x |
| 4 | 2911 us | 865 us | 3.36x |
| 8 | 5595 us | 1182 us | 4.73x |
| 16 | 11171 us | 2186 us | 5.11x |
| 32 | 22898 us | 4438 us | 5.15x |
| 64 | 45321 us | 9383 us | 4.83x |

Near-linear to 4 items (84% efficiency), then flattening to about **5.1x on 8
cores** (64%). One item is a small loss, as it must be: nothing to overlap and
the dispatch still gets paid.

### 2. Crossover (fan-out fixed at 8)

| work per item | sequential | parallel | speedup |
|---:|---:|---:|---:|
| 10 rounds | 0 us | 100 us | ~0x |
| 50 | 2 us | 111 us | 0.01x |
| 250 | 7 us | 88 us | 0.07x |
| 1250 | 43 us | 85 us | 0.50x |
| 6250 | 183 us | 85 us | 2.15x |
| 31250 | 1001 us | 203 us | 4.93x |

The pool costs a flat **85-110 us to dispatch and join a batch of 8**,
independent of the work size. Parity lands between 1250 and 6250 rounds, i.e.
around **10-20 us of work per item**, or roughly 100 us of total batch work.
Below that the pool is pure loss, and at trivial work sizes it is catastrophic
(100 us to do 0 us of work).

### 3. The combinator vs rolling it by hand

Same pool, same kernel, 32 items: `worker.map` against a hand-written
`worker.run` + `worker.wait` fan-out.

| run | `worker.map` | hand-rolled |
|---:|---:|---:|
| 1 | 943 us | 1000 us |
| 2 | 946 us | 952 us |
| 3 | 969 us | 954 us |

Identical within noise. The convenience is free; it is not sugar with a tax.

### 4. Leaks

`leaks(1)` over the map path: **0 leaks, 0 bytes**.

## Verdict

**Keep `worker.map`; the numbers justify it.** Real speedup on CPU-bound work,
no cost over the primitive it wraps, leak-clean.

**Do not add an automatic sequential fallback keyed on N.** The issue proposed
falling back below a measured threshold N, but the measurement shows the
threshold is not a function of N at all: it is a function of *work per item*,
which the runtime cannot know. At 8 items the layer is a 100x loss for 10-round
work and a 5x win for 31250-round work. Any N-based cutoff would be guessing at
the one variable that actually matters, and would guess wrong in both
directions. The honest interface is the documented crossover, which is now
written down: reach for `worker.map` when each item is worth more than about
20 us.

**A parallel fold is worth adding** on the same machinery, and these numbers
predict its behaviour: the same flat per-batch dispatch cost, the same
crossover, the same absence of a tax. It needs its own issue with the threshold
guidance above baked into the docs rather than into an automatic fallback.

## What was not measured

The issue also asked for a comparison against the same fan-out done with
`spawn` + `?`/reply. That was not run. Benchmark 3 compares against the
hand-written `worker.run` fan-out instead, which is the relevant "did the
combinator add a tax" question for this layer; the actor-path comparison is a
different runtime (scheduler cores rather than the worker pool) and belongs
with the fold work.

## Explicitly not adopted from Bend/HVM

The interaction-combinator engine, optimal reduction, and the GPU/CUDA backend.
They need the runtime Aether rejects. If a workload ever needs true HVM-grade
parallelism, the Aether-shaped move is to host HVM as a guest
(`contrib.host.*`) after a license check, not to reimplement it.
