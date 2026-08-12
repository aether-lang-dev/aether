# Fairness rules, and the audit that produced them

A cross-language benchmark is worth nothing if the implementations are not doing
the same work. This file records the rules the suite holds itself to, and the
audit findings that led to them, so that a future change can be checked against
something written down.

## Rules

1. **Standard library only.** Each implementation uses what ships with the
   language: pthreads and `<stdatomic.h>` for C, `std::thread` for C++,
   goroutines and channels for Go, `java.util.concurrent` for Java, `std` for
   Rust and Zig, OTP processes for Erlang and Elixir, language actors for Pony
   and Aether. No third-party actor framework, thread pool, or allocator.
2. **The same amount of concurrency.** Every implementation of a pattern
   creates the same number of concurrency units and passes the same number of
   messages. Where a pattern cannot express that across runtimes, the rule is
   the pattern's problem and the pattern gets changed, not the reporting.
3. **Divide by work actually performed.** A rate's denominator is a count of
   things the run really did. No implementation is credited for units it did not
   create.
4. **The same region is timed.** Setup that one language performs outside the
   timer must be outside it everywhere.
5. **Release-grade codegen everywhere.** LTO and optimisation settings are
   matched across compiled languages rather than left at defaults that differ.

## Audit, August 2026

### Skynet credited implementations for actors they never created

The pattern sums 1,000,000 leaves over a 10-ary tree. Every implementation
divided by the tree's full node count, 1,111,111, while creating wildly
different numbers of concurrency units:

| Implementations | Rule they used | Units created | Divisor used |
|---|---|---:|---:|
| Go, Erlang, Elixir | recurse to `size == 1` | 1,111,111 | 1,111,111 |
| Aether, Java, Scala | sequential below 100 | 11,111 | 1,111,111 |
| C, C++, Rust, Zig | threads for the top 3 levels | 1,111 | 1,111,111 |

So the languages that created the fewest units scored the highest, and the
reported figures were inflated 100x for Aether and 1000x for the four
thread-based languages. Measured before the fix:

```
C       Throughput:  72.02 M msg/sec     (1,111 threads)
Aether  Throughput: 225.05 M msg/sec     (11,111 actors)
Go      Throughput:   3.66 M msg/sec     (1,111,111 goroutines)
```

Go did a thousand times more concurrency work than C and appeared twenty times
worse at it. Aether's 61x margin over Go was almost entirely the divisor.

Equalising the unit count is what fixes it, and 1,111,111 is not the number to
equalise on: a pthread is not a goroutine, and this machine refuses the 4,096th
concurrent pthread.

```
created 4095 concurrent pthreads before failure
```

Every implementation now goes sequential below a subtree size of 1000, so all of
them create **1,111 units** and perform the same 1,000,000 leaf additions, and
each divides by the unit count it actually created. The result, all ten
implementations verified by running them, all returning the correct sum
499999500000:

| Language | ns per concurrency unit |
|---|---:|
| Aether | 471 |
| Go | 799 |
| Elixir | 1,194 |
| Erlang | 4,017 |
| Java | 9,249 |
| Zig | 14,410 |
| Rust | 14,537 |
| C | 15,084 |
| C++ | 20,214 |
| Scala (Akka) | 33,252 |

That ordering is a real result rather than an artifact: creating a unit of
concurrency costs about 15 microseconds when it is an OS thread and under a
microsecond when the runtime schedules it itself. It is also a far more modest
claim for Aether than the 61x the broken metric produced.

### Scala uses Akka, which breaks rule 1

All five Scala implementations import `akka.actor`. Akka is a third-party
framework, so Scala is not being measured on its standard library the way every
other language here is. It is not a hidden dependency, the README names it, but
it is still not base-versus-base. Tracked in #1532; the honest options are to rewrite
against `scala.concurrent` and `java.util.concurrent`, or to label the row as a
framework rather than a language.

### Pony implements three of five patterns

`benchmarks/cross-language/pony/` contains `counting`, `fork_join` and
`thread_ring`. There is no `ping_pong` and no `skynet`. The README claimed "All
11 languages implement all 5 patterns (55 total benchmarks, zero skips)"; the
real figure is 53, and the runner would fail trying to compile the two missing
directories. Corrected in the README; the gap itself is #1533.

### Ping-pong times different regions

Aether creates both actors before starting its timer. C, C++, Go and Rust start
the timer first and create their threads inside the measured region. For two
units against millions of messages the effect is small, but it is a systematic
bias in one direction and it breaks rule 4. Tracked in #1534.

### Zig printed a malformed rate

`zig/skynet.zig` assembled its throughput from an integer part and a fraction
and printed `0.+4 M msg/sec` for any rate below 1 M/sec, which the runner's
parser reads as garbage. The other four Zig benchmarks already used a float and
`{d:.2}`; skynet now does the same.

## Checking a change against these rules

- Unit counts: each skynet implementation prints `concurrency units: N`. All ten
  must print the same N for the same leaf count.
- Correctness: every implementation prints `Sum:`, which must be
  `499999500000` for 1,000,000 leaves. A wrong sum means work was skipped.
- Dependencies: `rust/Cargo.toml` has an empty `[dependencies]`;
  `go/*.go` import only the standard library; `scala/build.sbt` is the one
  exception and is documented above.
