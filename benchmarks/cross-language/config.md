# Benchmark Configuration Guide

## What the suite needs

`make benchmark` (from the repository root) builds Aether, builds the runner
(`run_benchmarks.ae`, in Aether) and runs every language whose toolchain it
finds, then serves the results page. `make benchmark-results` does the same
without the page. A language whose build fails is reported as skipped;
Aether is required.

| Language | Toolchain | Builds with |
|---|---|---|
| Aether | this repository (`make compiler ae stdlib`) and GCC or Clang | `aether/Makefile` |
| C, C++ | GCC or Clang | `c/Makefile`, `cpp/Makefile` |
| Go | `go` | `go/Makefile` (`go build`) |
| Rust | `cargo` | `rust/Makefile` (Cargo's release profile, `rust/.cargo/config.toml`) |
| Zig | `zig` 0.16 | `zig/Makefile` (`build.zig`, `ReleaseFast`) |
| Java | JDK 21 or later (virtual threads) | `java/Makefile` (`javac`) |
| Scala | `sbt` and JDK 21 or later | `scala/Makefile` (`sbt compile`) |
| Erlang | Erlang/OTP (`erlc`, `erl`) | `erlang/Makefile` |
| Elixir | `elixir` | `elixir/Makefile` (scripts, compiled as they load) |
| Pony | `ponyc` | `pony/Makefile` |

CPU time and peak memory need GNU `time` at `/usr/bin/time` on Linux (the
`time` package) or the system one on macOS. On Windows they read n/a.

`make flags` in a language's directory prints how it builds; the results
record it, with the toolchain's version.

## Settings

The runner reads its settings from the environment:

| Variable | Default | Meaning |
|---|---|---|
| `BENCHMARK_MESSAGES` | 1000000 | Messages per run (skynet: leaves) |
| `BENCH_RUNS` | 5 | Timed runs per language and pattern; the median is reported |
| `BENCH_WARMUP` | 1 | Discarded runs before them, the same for every language |
| `BENCH_SKYNET_RUNS` | 25 | Timed runs for skynet, whose run is about 0.3 ms (#3868); never fewer than `BENCH_RUNS` |
| `BENCH_LANGS` | all | Languages to run, comma-separated (`aether,c,rust`) |
| `BENCH_PATTERNS` | all | Patterns to run, comma-separated (`ping_pong,skynet`) |
| `BENCH_STRICT` | 0 | 1: exit 1 if any selected language does not build or is wrong (the CI job) |

```bash
BENCHMARK_MESSAGES=100000 BENCH_RUNS=3 make benchmark
BENCH_LANGS=aether,go BENCH_PATTERNS=thread_ring make benchmark
```

Around 100,000 messages suits a small machine; ten million gives longer,
steadier runs. A rate is only worth quoting with its range: see
[FAIRNESS.md](FAIRNESS.md).

## Installing toolchains

macOS:

```bash
brew install go rust zig erlang elixir ponyc sbt openjdk@21
```

Ubuntu or Debian:

```bash
sudo apt install golang rustc cargo erlang elixir openjdk-21-jdk time
```

Zig 0.16 comes from ziglang.org, Pony from `ponyup`, and sbt from its own
package repository; a distribution's packages may be older than the suite
needs.

## Troubleshooting

- **A language is skipped.** Its build log is in `build/<language>_<pattern>.log`;
  `cd <language> && make <pattern>` reproduces it.
- **A language is `[FAIL]`.** Its result was wrong, it exited with an error,
  or it printed no rate; the reason is in the results and on the page.
- **Out of memory.** Lower `BENCHMARK_MESSAGES`.
