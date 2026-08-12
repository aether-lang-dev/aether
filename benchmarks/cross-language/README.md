# Aether Cross-Language Benchmark Suite

Comparative benchmarking of actor/message-passing implementations across 11 programming languages, based on the [Savina Actor Benchmark Suite](https://dl.acm.org/doi/10.1145/2687357.2687368) (Imam & Sarkar, Rice University, 2014).

## Quick Start

```bash
make benchmark    # From repo root — builds, runs, opens visualization
```

Both the benchmark runner (`run_benchmarks.ae`) and the visualization server (`visualize/server.ae`) are written in Aether using only the stdlib — no extern FFI, no C helper files. The runner compiles all 11 languages, runs each benchmark, parses output, computes statistics, and writes JSON results. The server serves the interactive dashboard over HTTP.

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
| **Aether** | Native (Aether runtime) | Lock-free SPSC actors, computed goto dispatch |
| **C** | Native (pthread) | pthread mutex + condvar |
| **C++** | Native (std::thread) | std::mutex + std::condition_variable |
| **Go** | Go runtime | Goroutines with channels |
| **Rust** | Native (std::sync) | sync_channel (bounded MPSC) |
| **Java** | JVM | BlockingQueue / ForkJoinPool |
| **Zig** | Native | std.Thread + Mutex |
| **Elixir** | BEAM VM | Lightweight processes + mailboxes |
| **Erlang** | BEAM VM | Lightweight processes + mailboxes |
| **Pony** | Native (Pony runtime) | GC-free actors, ref capabilities |
| **Scala** | JVM (Akka) | Akka actor system, the one third-party dependency here; see [FAIRNESS.md](FAIRNESS.md) |

53 of the 55 combinations exist. Pony implements `counting`, `fork_join` and
`thread_ring` only; it has no `ping_pong` and no `skynet` directory.

See [FAIRNESS.md](FAIRNESS.md) for the rules the suite holds itself to, and for
the August 2026 audit that found the skynet metric crediting every
implementation for actors it never created.

## Methodology

### Statistical approach

- Each benchmark runs **5 times** (configurable: `BENCH_RUNS=N`)
- **Median** throughput reported (robust to outliers)
- JVM/BEAM languages get **1 warmup run** (discarded) before timed measurement (`BENCH_WARMUP=N`)
- All individual run values stored in results JSON

### Metrics

| Metric | Description |
|--------|-------------|
| **Throughput** (M msg/s) | Messages per second (median of N runs) |
| **Latency** (ns/msg) | Nanoseconds per message |
| **Memory** (MB) | Peak resident set size (RSS) |
| **Relative** (%) | Percentage of fastest language |
| **CV%** | Coefficient of variation (σ/μ × 100) — run-to-run stability. Standard statistical bands drive colour coding: low / moderate / high variance, independent of which language is being measured. |
| **Range** | Min–Max throughput across runs |
| **Efficiency** | Throughput per MB of memory |

### Fairness

- All languages use the same message count (`BENCHMARK_MESSAGES`)
- All compiled with highest optimization flags (`-O3`, `--release`, etc.)
- Skynet throughput standardized: **total tree nodes / elapsed time** across all languages
- Each language uses its idiomatic concurrency model
- C/C++/Zig use standard thread primitives (no actor framework) — this is intentionally honest: raw thread synchronization vs purpose-built actor runtimes

### Measurement

- Wall-clock time via `CLOCK_MONOTONIC`
- Memory via `/usr/bin/time` (macOS: `-l`, Linux: `-v`)
- Correctness validated (e.g., skynet sum = 499,999,500,000)

## Configuration

```bash
# Message count
BENCHMARK_MESSAGES=10000000 make benchmark

# Number of timed runs
BENCH_RUNS=10 make benchmark

# Warmup runs for JIT languages
BENCH_WARMUP=3 make benchmark
```

Or edit `benchmark_config.json`:

```json
{
  "messages": 1000000,
  "timeout_seconds": 60
}
```

## Visualization

After running, open `http://localhost:8080` for an interactive dashboard:

- **Summary**: Aether rank, throughput, efficiency, spread
- **Charts**: Throughput and memory bar charts
- **Sortable table**: Click any column header — throughput, latency, memory, CV%, efficiency
- **Pattern tabs**: Switch between all 5 patterns
- **Methodology**: Inline explanation of metrics and color coding

Results stored as JSON in `visualize/results_*.json` with full metadata.

## What This Does NOT Measure

- I/O performance or network throughput
- Garbage collection behavior under sustained load
- Real-world application workloads
- Distributed (multi-node) messaging

## References

- [Savina — An Actor Benchmark Suite](https://dl.acm.org/doi/10.1145/2687357.2687368) (Imam & Sarkar, 2014)
- [Skynet Actor Benchmark](https://github.com/atemerev/skynet) (Temerev)
- [Computer Language Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/)
- [tzcnt/runtime-benchmarks](https://github.com/tzcnt/runtime-benchmarks) — C++ tasking library comparison
