# Aether Performance Benchmarks

## Overview

The Aether benchmark suite measures message-passing throughput, latency, memory efficiency, and scalability across 11 programming languages using 5 patterns from the [Savina Actor Benchmark Suite](https://dl.acm.org/doi/10.1145/2687357.2687368) (Imam & Sarkar, Rice University, 2014).

## Benchmark Patterns

### Ping-Pong

Two actors exchange messages back and forth. Measures pure message-passing latency with zero computation.

- 2 actors, configurable message count via `BENCHMARK_MESSAGES`
- Tests: message send/receive overhead, mailbox performance

**Location:** `benchmarks/cross-language/aether/ping_pong.ae`

### Counting

Single actor counts incoming messages from the main thread. Tests unidirectional throughput and single-actor scheduler optimizations.

- 1 actor, main thread sends all messages
- Activates Main Thread Actor Mode (synchronous processing, no scheduler overhead)

**Location:** `benchmarks/cross-language/aether/counting.ae`

### Thread Ring

Token passed through a ring of 100 actors with sequential dependencies. Measures scheduling overhead with many actors.

- 100 actors in a ring, token forwarded around
- Benefits from locality-aware placement (all actors on same core)

**Location:** `benchmarks/cross-language/aether/thread_ring.ae`

### Fork-Join

Round-robin fan-out to 8 worker actors. Tests parallel dispatch throughput and batch send optimization.

- 8 worker actors, messages distributed round-robin from main thread
- Activates Batch Send optimization (groups messages by target core)

**Location:** `benchmarks/cross-language/aether/fork_join.ae`

### Skynet

Recursive 10-ary tree summing 1M leaves. Tests actor creation rate, message aggregation, and tree coordination.

- Recursive tree: each node spawns 10 children, leaves compute partial sums
- Each language uses its idiomatic concurrency model (threads, goroutines, actors, processes)
- Throughput measured as **total tree nodes / elapsed time** for fair cross-language comparison

**Location:** `benchmarks/cross-language/aether/skynet.ae`

## Languages

All 11 languages implement all 5 patterns:

| Language | Runtime | Concurrency Model | Optimization |
|----------|---------|-------------------|-------------|
| **Aether** | Native (Aether runtime) | Lock-free SPSC actors | `-O3 -march=native` |
| **C** | Native (pthread) | pthread mutex + condvar | `-O3 -march=native` |
| **C++** | Native (std::thread) | std::mutex + std::condition_variable | `-O3 -std=c++17 -march=native` |
| **Go** | Go runtime | Goroutines with channels | Default (Go always optimizes) |
| **Rust** | Native (std::sync) | sync_channel (bounded MPSC) | `--release` |
| **Java** | JVM | BlockingQueue / ForkJoinPool | Default |
| **Zig** | Native | Mutex + Condition variable | `ReleaseFast` |
| **Elixir** | BEAM VM | Lightweight processes + mailboxes | Default |
| **Erlang** | BEAM VM | Lightweight processes + mailboxes | Default |
| **Pony** | Native (Pony runtime) | GC-free actors, ref capabilities | Default |
| **Scala** | JVM (Akka) | Akka actor system | Default |

## Methodology

### Statistical Approach

Each benchmark is run **5 times** (configurable via `BENCH_RUNS`). The **median** throughput is reported to reduce the impact of outliers. All individual run values are stored for transparency.

**JVM/BEAM warmup:** Java, Scala, Erlang, and Elixir receive 1 warmup run (discarded, not timed) before the measured runs. This allows JIT compilation to complete before measurement, matching how these runtimes perform in production.

**Metrics reported per language:**

| Metric | Description |
|--------|-------------|
| Throughput (M msg/s) | Messages processed per second (median of N runs) |
| Latency (ns/msg) | Nanoseconds per message (median) |
| Memory (MB) | Peak resident set size via `/usr/bin/time` |
| Relative (%) | Percentage of the fastest language's throughput |
| CV% | Coefficient of variation (σ/μ × 100), measures run-to-run stability |
| Range | Min–Max throughput across all runs |
| Efficiency | Throughput per MB of memory (M msg/s/MB) |

**CV% interpretation.** The thresholds below are general statistical
stability bands for coefficient of variation, not Aether-specific
targets. They tell you how much to trust a single run's headline
figure, regardless of which language is being measured:

- **< 5%** (green): run-to-run variance is small; reported medians are trustworthy.
- **5–15%** (orange): moderate variance; read medians alongside Min–Max range.
- **> 15%** (red): high variance; investigate thermal throttling, OS scheduling, or shared-host noise before citing numbers.

The runner computes CV in 64-bit and refuses to publish a negative
value. Both matter for the fastest patterns: throughput in the hundreds
of millions of msg/sec makes `range * 50` exceed the 32-bit ceiling
before the divide, which used to yield a NEGATIVE CV for exactly the
results most likely to be quoted. A negative coefficient of variation is
arithmetically impossible, so if one ever appears the runner reports it
and substitutes zero rather than letting a consumer read it as a
stability signal.

### Measurement

- Wall-clock time via `CLOCK_MONOTONIC` (not CPU time)
- All compiled languages use highest optimization flags
- Correctness validated (e.g., skynet sum = 499,999,500,000 for 1M leaves)
- Memory measured as peak RSS via `/usr/bin/time -l` (macOS) or `/usr/bin/time -v` (Linux)

### Fairness

- All languages use the same message count (`BENCHMARK_MESSAGES` environment variable)
- Skynet throughput standardized: all languages count **total tree nodes** (not messages or threads)
- Each language uses its idiomatic concurrency model, the benchmark measures "how well does this language solve the problem," not "how identical is the implementation"
- Results JSON includes full methodology metadata for reproducibility

## Running Benchmarks

### Full Suite (all languages)

```bash
make benchmark
```

This builds the Aether benchmark runner (`run_benchmarks.ae`), compiles all 11 languages, runs all 5 patterns, writes JSON results, and launches the visualization server (`server.ae`) at `http://localhost:8080`. Both the runner and the server are written in Aether using only the stdlib, no extern FFI, no C helper files.

### Aether Only

```bash
cd benchmarks/cross-language/aether
make ping_pong && ./ping_pong
```

### Configuration

```bash
# Override message count
BENCHMARK_MESSAGES=10000000 make benchmark

# Override number of timed runs
BENCH_RUNS=10 make benchmark

# Override warmup runs for JIT languages
BENCH_WARMUP=3 make benchmark
```

### Visualization

After `make benchmark`, a web UI opens at `http://localhost:8080` showing:

- **Summary strip**: Aether rank, throughput, vs fastest, efficiency, spread
- **Charts**: Throughput and memory bar charts with Aether highlighted
- **Sortable table**: Click any column header to sort. Columns include throughput, latency, memory, relative performance bar, CV%, min-max range, and efficiency
- **Pattern tabs**: Switch between all 5 benchmark patterns
- **Methodology box**: Explains what's being measured and how to interpret CV%

Results are stored as JSON in `benchmarks/cross-language/visualize/results_*.json` with full metadata (hardware, runs, warmup, methodology).

## Active Optimizations

These Aether runtime optimizations affect benchmark performance:

| Optimization | Effect | File |
|-------------|--------|------|
| Main Thread Actor Mode | Single-actor programs bypass scheduler entirely | `runtime/actors/aether_send_message.c` |
| Inline message path | Single-field messages (int, int64, uint64, duration, ptr) skip heap allocation, value stored in `Message.payload_int` | `compiler/codegen/codegen.c` |
| Batch send | Main-thread fan-out groups messages by target core, reducing atomics from N to num_cores | `runtime/scheduler/multicore_scheduler.c` |
| Partial batch enqueue | `queue_enqueue_batch` returns how many fit instead of all-or-nothing | `runtime/scheduler/lockfree_queue.h` |
| Work inlining | Same-core sends invoke `actor->step()` immediately, skipping the scheduler drain loop | `runtime/scheduler/multicore_scheduler.c` |
| Computed goto dispatch | Message handlers use GCC computed-goto dispatch tables | `compiler/codegen/codegen_actor.c` |
| TLS caching | `current_core_id` cached in local variables, avoiding repeated `tlv_get_addr` on macOS | `runtime/scheduler/multicore_scheduler.c` |
| SPSC queues | Lock-free single-producer single-consumer ring buffers for cross-core messaging | `runtime/scheduler/lockfree_queue.h` |
| Adaptive batching | Batch size adjusts dynamically (64–1024) based on queue utilization | `runtime/actors/aether_adaptive_batch.h` |

## References

- [Savina, An Actor Benchmark Suite](https://dl.acm.org/doi/10.1145/2687357.2687368) (Imam & Sarkar, 2014)
- [Cross-Language Benchmark Source](../benchmarks/cross-language/)
- [Runtime Optimizations](runtime-optimizations.md)
- [Scheduler Architecture](scheduler-quick-reference.md)
