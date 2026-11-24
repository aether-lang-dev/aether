# Aether Cross-Language Benchmark Suite

Comparative benchmarking of actor/message-passing implementations across 11 programming languages, based on the [Savina Actor Benchmark Suite](https://dl.acm.org/doi/10.1145/2687357.2687368) (Imam & Sarkar, Rice University, 2014).

## Quick Start

```bash
make benchmark    # From repo root: builds, runs, opens visualization
```

Both the benchmark runner (`run_benchmarks.ae`) and the visualization server (`visualize/server.ae`) are written in Aether using only the stdlib: no extern FFI, no C helper files. The runner compiles all 11 languages, runs each benchmark, parses output, computes statistics, and writes JSON results. The server serves the interactive dashboard over HTTP.

## Patterns (5)

| Pattern | What It Measures |
|---------|-----------------|
| **Ping-Pong** | Pure message-passing latency between two actors |
| **Counting** | Single-actor unidirectional throughput |
| **Thread Ring** | Scheduling overhead with 100 actors in a ring |
| **Fork-Join** | Parallel fan-out throughput to 8 workers |
| **Skynet** | Recursive 10-ary tree summing 1M leaves, actor creation + aggregation. Every language goes sequential below a subtree of 1000, so all create the same 1,111 concurrency units |

## Languages (11)

| Language | Runtime | Concurrency Model |
|----------|---------|-------------------|
| **Aether** | Native (Aether runtime) | Actors on the runtime's scheduler, one thread per core |
| **C** | Native (pthread) | A thread per unit; single-sender mailboxes on C11 atomics |
| **C++** | Native (std::thread) | A thread per unit; single-sender mailboxes on `std::atomic` |
| **Go** | Go runtime | Goroutines with buffered channels |
| **Rust** | Native (std::thread) | A thread per unit; single-sender mailboxes on `std` atomics |
| **Java** | JVM | Platform threads and `LinkedTransferQueue`; virtual threads for the ring; `ForkJoinPool` for skynet |
| **Zig** | Native (std.Thread) | A thread per unit; single-sender mailboxes on `std.atomic` |
| **Elixir** | BEAM VM | Lightweight processes + mailboxes |
| **Erlang** | BEAM VM | Lightweight processes + mailboxes |
| **Pony** | Native (Pony runtime) | GC-free actors, ref capabilities |
| **Scala** | JVM | As Java, through the same `java.util.concurrent` |

All 11 languages implement all 5 patterns, 55 in total.

They make two kinds of entry, and each is ranked only against its own kind (`FAIRNESS.md`, "Two tiers"): **runtimes** (Aether, Go, Erlang, Elixir, Pony), which schedule their own units and own their unbounded mailboxes, and **threads + queue** (C, C++, Rust, Zig, Java, Scala), OS threads joined by a queue. The second tier is the floor a runtime is read against, not its competition.

See [FAIRNESS.md](FAIRNESS.md) for the rules the suite holds itself to and the
audits that produced them. In short: standard library only, the same number of
concurrency units and the same sequential work in every implementation, rates
divided by work actually performed, the same region timed, medians rather than
single runs, every result checked, every language built by its own build file
at its release level, the same warmup for all, and the technique an expert
would use, fixed by the shape of the work rather than picked per pattern.

## Methodology

### Statistical approach

- Each benchmark runs **5 times** (`BENCH_RUNS=N`) after **1 warmup run**
  (`BENCH_WARMUP=N`), the same for every language. Skynet runs **25 times**
  (`BENCH_SKYNET_RUNS=N`): one run takes about 0.3 ms, short enough that a
  median of five is mostly scheduling jitter (#3868)
- The **median** of the timed runs is reported, with the minimum, the maximum
  and the coefficient of variation (sample standard deviation over the mean)
- Every run's value is stored in the results JSON

### Metrics

| Metric | Description |
|--------|-------------|
| **Throughput** (M msg/s) | Messages per second, median of the runs |
| **Per CPU-s** (M msg/CPU-s) | Messages per second of CPU time (user + system, every thread): what a message cost, which wall-clock throughput hides for a runtime that spins |
| **Latency** (ns/msg) | Nanoseconds per message |
| **CPU s** | CPU time of a run |
| **Memory** (MB) | Peak resident set size (RSS) |
| **Relative** (%) | Percentage of the fastest language of the same tier |
| **CV%** | Coefficient of variation, the run-to-run stability. Standard statistical bands drive the colour coding: low, moderate or high variance, whatever the language |
| **Range** | Minimum and maximum throughput across the runs |
| **Efficiency** | Throughput per MB of memory |

CPU time and memory come from `/usr/bin/time` (`-v` on Linux, `-l` on macOS)
and read n/a on Windows, where MSYS's `time` cannot see a native program.

### Fairness

- Standard library only, no frameworks; the same message count
  (`BENCHMARK_MESSAGES`) and the same units and messages in every language
- Each language is built by its own build file (`make <pattern>` in its
  directory) at its release level for the host CPU: `-O3 -march=native
  -flto` for C, C++ and Aether, Cargo's release profile with fat LTO and
  `target-cpu=native` for Rust, `ReleaseFast` for Zig, the defaults of
  `go build`, `ponyc`, `javac`, `sbt`, `erlc` and `elixir`; `make flags`
  in each directory prints what it uses, and the results record it
- Every run's checksum (and skynet's unit count) is checked; a wrong one is
  reported as `[FAIL]` and not measured
- Skynet divides by the units created, 1,111 for a million leaves everywhere
- The technique per language follows a rule set by the shape of the work:
  the thread-per-unit languages (C, C++, Rust, Zig) share one mailbox design
  that spins, then sleeps, where the units fit the cores and sleeps at once
  where they do not; Java and Scala use platform threads with
  `LinkedTransferQueue`, and virtual threads for the oversubscribed ring.
  FAIRNESS.md has each language's rule and every alternative measured

### Measurement

- Wall-clock time from each language's monotonic clock, around the creation
  of the units and all the messages
- CPU time and peak memory via `/usr/bin/time`
- Correctness: every result checked against the value a correct run prints

## Configuration

```bash
# Message count (default 1,000,000)
BENCHMARK_MESSAGES=10000000 make benchmark

# Timed runs per language and pattern (default 5)
BENCH_RUNS=10 make benchmark

# Warmup runs, the same for every language (default 1)
BENCH_WARMUP=3 make benchmark

# A subset
BENCH_LANGS=aether,c,rust BENCH_PATTERNS=ping_pong,skynet make benchmark
```

See [config.md](config.md) for the toolchains each language needs.

## Visualization

After running, open `http://localhost:8080` for an interactive dashboard:

- **Summary**: Aether's rank among the runtimes (and overall), throughput, efficiency, spread
- **Tiers**: each row's tier; ranks and Relative are within it
- **Charts**: Throughput and memory bar charts
- **Sortable table**: Click any column header: throughput, latency, memory, CV%, efficiency
- **Pattern tabs**: Switch between all 5 patterns
- **Methodology**: Inline explanation of metrics and color coding

Results stored as JSON in `visualize/results_*.json` with full metadata.

## What This Does NOT Measure

- I/O performance or network throughput
- Garbage collection behavior under sustained load
- Real-world application workloads
- Distributed (multi-node) messaging

## References

- [Savina: An Actor Benchmark Suite](https://dl.acm.org/doi/10.1145/2687357.2687368) (Imam & Sarkar, 2014)
- [Skynet Actor Benchmark](https://github.com/atemerev/skynet) (Temerev)
- [Computer Language Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/)
- [tzcnt/runtime-benchmarks](https://github.com/tzcnt/runtime-benchmarks), a C++ tasking library comparison
