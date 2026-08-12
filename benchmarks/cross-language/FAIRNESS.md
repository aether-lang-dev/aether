# Fairness rules, and the audit that produced them

A cross-language benchmark is worth nothing if the implementations are not doing
the same work. This file records the rules the suite holds itself to and the
audit findings that led to them, so a future change can be checked against
something written down rather than against nobody's memory.

## Rules

1. **Standard library only.** Each implementation uses what ships with the
   language: pthreads and `<stdatomic.h>` for C, `std::thread` for C++,
   goroutines and channels for Go, `java.util.concurrent` for Java, `std` for
   Rust and Zig, OTP processes for Erlang and Elixir, language actors for Pony
   and Aether. No third-party actor framework, thread pool, or allocator.
2. **The same amount of concurrency.** Every implementation of a pattern creates
   the same number of concurrency units and passes the same number of messages.
   Where a pattern cannot express that across runtimes, the pattern gets
   changed, not the reporting.
3. **The same sequential work.** The part that is not concurrent has to match in
   operation count too: a plain accumulator loop everywhere, not a loop in one
   language, a recursive descent in another and a list allocation in a third.
4. **Divide by work actually performed.** A rate's denominator counts things the
   run really did. No implementation is credited for units it did not create.
5. **The same region is timed.** Setup that one language performs outside the
   timer must be outside it everywhere.
6. **Report medians, with the spread.** A single run on a shared machine is not a
   measurement. On this hardware the thread-based languages vary by 20% run to
   run, which is wider than several of the gaps between them.

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

So the implementations that created the fewest units scored the highest, by a
factor of 100 for Aether and 1000 for the four thread-based languages. Measured
before the fix:

```
C       Throughput:  72.02 M msg/sec     (1,111 threads)
Aether  Throughput: 225.05 M msg/sec     (11,111 actors)
Go      Throughput:   3.66 M msg/sec     (1,111,111 goroutines)
```

Go did a thousand times more concurrency work than C and appeared twenty times
worse at it.

1,111,111 is not the number to equalise on, because a pthread is not a
goroutine:

```
created 4095 concurrent pthreads before failure
```

Every implementation now goes sequential below a subtree size of 1000, so all of
them create **1,111 units**, and each divides by the count it created. Each also
prints that count, so the invariant is checkable rather than assumed.

### The sequential half was not equal either

Once the unit counts matched, the remaining difference was in the part that is
not concurrent. C, C++, Go, Rust and Zig summed a leaf subtree by recursing
10-ary down to size 1, which is about 11% more calls than there are values, plus
the call overhead. Erlang built a 1000-element list per leaf and summed it, and
that cost it 2.7x: 4,017 ns per unit against 1,507 once the list was gone.
Aether, Java and Scala already used an accumulator loop.

All ten now use a plain accumulator loop over the subtree, so the sequential
operation count is identical.

### Results

Median of five runs each, three for Scala, 1,000,000 leaves, 1,111 units, on an
8-core M1 Pro. Every implementation returns the correct sum, 499999500000.

| Language | ns per concurrency unit | Range |
|---|---:|---|
| Go | 490 | 435-533 |
| Aether | 516 | 402-558 |
| Erlang | 990 | 919-1012 |
| Elixir | 1,096 | 950-1124 |
| C | 10,534 | 9369-11439 |
| Zig | 11,700 | 11468-13004 |
| C++ | 11,745 | 10459-12036 |
| Rust | 12,386 | 11907-12517 |
| Java | 13,968 | 10043-14498 |
| Scala (Akka) | 38,322 | 35485-39661 |

Read that as two groups rather than ten positions. Go, Aether, Erlang and Elixir
schedule their own units and pay roughly half a microsecond to a microsecond for
one. C, Zig, C++, Rust and Java hand the work to the operating system or to a
thread pool and pay ten to fourteen. Akka sits on its own.

**Go and Aether are indistinguishable on this pattern.** Their medians differ by
5% and their ranges overlap almost entirely, so the suite does not show Aether
ahead of Go here. Single runs earlier said Aether 471 against Go 799, which
looked like a 1.7x win and was noise.

### Scala uses Akka, which breaks rule 1

All five Scala implementations import `akka.actor`, and `scala/build.sbt` pulls
`akka-actor` 2.8.5. Akka is a third-party framework, so the Scala column is not
measured on its standard library the way the other ten are. It is disclosed
rather than hidden, the README names it, but it is not base-versus-base.

Awkward, because `scala.actors` was removed in 2.13, so Scala's own library has
no actor system: base Scala for this suite would mean `scala.concurrent` or
`java.util.concurrent`, which is what the Java column already measures. Open in
the issue tracker.

### Pony implements three of five patterns

`pony/` contains `counting`, `fork_join` and `thread_ring`. There is no
`ping_pong` and no `skynet`. The README claimed all eleven languages implement
all five patterns with zero skips; the real figure is 53 of 55, and the runner
would fail trying to compile the two missing directories.

### Ping-pong times different regions

Aether creates both actors before starting its timer. C, C++, Go and Rust start
the timer first and create their threads inside the measured region. Two units
against millions of messages puts this well under a percent, so it is not why
any column wins, but it breaks rule 5 and the fix is to move two lines.

### Zig printed a malformed rate

`zig/skynet.zig` assembled its throughput from an integer and a fraction and
printed `0.+4 M msg/sec` for any rate below 1 M/sec, which the runner's parser
reads as garbage. The other four Zig benchmarks already used a float and
`{d:.2}`.

Zig prints through `std.debug.print`, which goes to stderr. The runner captures
with `2>&1` so this is fine, but a harness reading only stdout collects nothing
from Zig and shows no error.

## Checking a change against these rules

- **Unit counts.** Each skynet implementation prints `concurrency units: N`. All
  ten must print the same N for the same leaf count.
- **Correctness.** Every implementation prints `Sum:`, which must be
  `499999500000` for 1,000,000 leaves. A wrong sum means work was skipped.
- **Dependencies.** `rust/Cargo.toml` has an empty `[dependencies]`; the Go
  files import only the standard library; `scala/build.sbt` is the one exception
  and is described above.
- **Spread.** Run each at least five times. If the gap between two languages is
  smaller than either one's run-to-run range, the suite has not shown a
  difference between them.
