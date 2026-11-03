# Aether Benchmarks

## Which one to run

| Question | Run |
|---|---|
| Did my change make Aether faster or slower? | `make bench-core-compare BASE=<tree at the merge base>` |
| How fast is Aether on its own today? | `make bench-core` |
| How does Aether's actor runtime compare with other languages? | `make benchmark` |
| How fast is the JSON parser against yyjson? | `make bench-json-compare` |

## Directories

```
benchmarks/
├── core/             # sends, asks, idle actors, strings, maps, JSON, ae and
│                     #   aetherc, run against the merge base (bench-core)
├── ownership/        # the ownership model's micro-benchmarks (#2925),
│                     #   timed whole by core/run.ae
├── cross-language/   # Savina patterns in 11 languages (make benchmark)
├── json/             # std.json parser against a corpus, yyjson beside it
├── langarena/        # packed-float kernels against same-machine Go
├── concurrent-cache/ # single-owner actor vs sharded actor map vs COW cell
├── fork-join/        # parallel map crossover points
└── http/             # HTTP server and load balancer experiments
```

## Core benchmarks (`benchmarks/core`)

The programs a change to the runtime, std or compiler is measured with. Each
program prints `row <name> <value> <unit>` lines, and `core/run.ae` builds
each one with a tree's own `ae`, runs it `BENCH_RUNS` times (default 5), and
keeps the best value of every row. Lower is better in every row.

```sh
# this tree only
make bench-core

# against the merge base, the two trees' runs interleaved
git worktree add ../base $(git merge-base HEAD origin/main)
make -C ../base compiler ae stdlib
make bench-core-compare BASE=../base
```

`BENCH_ONLY=send,strings` picks programs, `BENCH_OWNERSHIP=,` leaves out the
ownership programs, and the table also goes to `build/bench-core.tsv`. Rows:

- **send**: ns per message for a one-int, a two-int and a string field
  message, the string forwarded as received or built fresh per send
- **ask**: ns per ask round trip, from main and from inside a handler
- **idle**: ns per message of one ping-pong pair beside 0, 1k and 100k idle
  actors
- **strings**: concat, interpolation, substring, index_of through 256 KiB,
  split of a 2000 field line
- **maps**: put and get with string keys
- **json**: building a 5000 key object and a 300 deep nesting, encoding the
  object
- **alloc_par**: strings built and dropped by 1 and by 8 actors at once
- **ae and aetherc**: `ae run` on a warm cache, a cold `ae build` of a std
  heavy program, and aetherc's phases of that compile (`--time-phases`)
- **ownership**: each `benchmarks/ownership` program, timed whole

Numbers on a shared machine move by a few percent between runs; compare
best-of-N against best-of-N, and look again at any row that moved by less
than about 5%.

## Cross-language benchmarks

`make benchmark` runs the cross-language suite and serves the results at
<http://localhost:8080>. The rules each language follows are in
[cross-language/FAIRNESS.md](cross-language/FAIRNESS.md), the method in
[cross-language/README.md](cross-language/README.md).

## Other suites

- [json/](json/): `make bench-json`, `make bench-json-compare`, and
  [json/baseline.md](json/baseline.md) for recorded numbers.
- [langarena/](langarena/README.md): the Matmul, Nbody, NeuralNet and
  Spectralnorm cluster against same-machine Go, with an actor-parallel matmul.
- [concurrent-cache/](concurrent-cache/concurrent_cache_bench.ae): read/write
  mixes over three cache designs, see
  [the design note](../docs/design/concurrent-cache-benchmark.md).

Results depend on the machine: run them on your own hardware before drawing
conclusions.
