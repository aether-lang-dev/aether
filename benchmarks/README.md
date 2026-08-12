# Aether Benchmarks

## Quick Start

```bash
# Run cross-language benchmark suite with interactive UI
make benchmark
# Open http://localhost:8080 to view results
```

The benchmark suite compares Aether's actor implementation against C, C++, Go, Rust, Java, Zig, Erlang, Elixir, Pony, and Scala using baseline implementations.

**Full documentation:** [benchmarks/cross-language/README.md](cross-language/README.md)

**Concurrent cache designs:** [`concurrent-cache/concurrent_cache_bench.ae`](concurrent-cache/concurrent_cache_bench.ae): single-owner-actor vs. sharded-actor-map vs. COW snapshot cell, across read/write workload mixes (see [`docs/concurrent-cache-benchmark.md`](../docs/design/concurrent-cache-benchmark.md)).

## Directory Structure

```
benchmarks/
├── cross-language/    # Multi-language comparative benchmarks (PRIMARY)
├── optimizations/     # Runtime optimization experiments
├── rejected/          # Failed optimization attempts
├── infrastructure/    # Testing infrastructure
└── ideas/            # Potential future optimizations
```

## Cross-Language Benchmarks

The primary benchmark suite (`cross-language/`) provides fair comparisons across 11 languages:

**Languages tested:**
- Aether
- C (pthreads)
- C++
- Go
- Rust
- Java
- Zig
- Erlang
- Elixir
- Pony
- Scala (Akka)

**Test characteristics:**
- Ping-pong pattern with full round-trip validation
- All languages use standard optimizations
- No specialized tuning
- Configurable message counts
- Interactive visualization UI

See [cross-language/README.md](cross-language/README.md) for methodology and detailed documentation.

## Runtime Optimization Experiments

The `optimizations/` directory contains experimental benchmarks exploring various runtime optimization techniques. These are research experiments and not production benchmarks.

**Implemented optimizations:**
1. Partitioned multicore scheduler
2. Message coalescing
3. Zero-copy message passing
4. Type-specific memory pools
5. Lock-free mailboxes
6. SIMD batch processing
7. Inline assembly atomics
8. Computed goto dispatch

**Rejected optimizations:**
- Manual prefetch hints (negative impact)
- Profile-guided optimization (negative impact)
- Power-of-2 masking (no benefit)

These experiments help understand what works and what doesn't in actor runtime design.

## Running Benchmarks

### Cross-Language Suite (Recommended)

```bash
make benchmark
```

This runs the full comparative benchmark suite and launches an interactive web UI.

### Individual Optimization Tests

```bash
cd benchmarks/optimizations
gcc bench_message_coalescing.c -o test -O2
./test
```

## Important Notes

- Results are highly system-dependent
- Experiments are for research purposes
- Cross-language suite provides fair comparative analysis
- Run on your own hardware to evaluate performance
