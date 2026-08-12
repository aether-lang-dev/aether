# Fairness rules, and the audit that produced them

A cross-language benchmark is worth nothing if the implementations are not doing
the same work. This file records the rules the suite holds itself to and the
audit findings that led to them, so a future change can be checked against
something written down rather than against nobody's memory.

## Rules

1. **Standard library only.** Each implementation uses what ships with the
   language: pthreads and `<stdatomic.h>` for C, `std::thread` for C++,
   goroutines and channels for Go, `java.util.concurrent` for Java and Scala,
   `std` for Rust and Zig, OTP processes for Erlang and Elixir, language actors
   for Pony and Aether. No third-party actor framework, thread pool or
   allocator, and no exceptions.
2. **The same amount of concurrency.** Every implementation of a pattern creates
   the same number of concurrency units and passes the same number of messages.
   Where a pattern cannot express that across runtimes, the pattern gets
   changed, not the reporting.
3. **The same sequential work.** The part that is not concurrent has to match in
   operation count too: a plain accumulator loop everywhere, not a loop in one
   language, a recursive descent in another and a list allocation in a third.
4. **Divide by work actually performed.** A rate's denominator counts things the
   run really did. No implementation is credited for units it did not create.
5. **The same region is timed.** For every pattern the clock starts before the
   concurrency units are created, so creation is measured everywhere or nowhere.
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

All eleven now use a plain accumulator loop over the subtree, so the sequential
operation count is identical.

### Results

Median of five runs, three for Scala, 1,000,000 leaves, 1,111 units. Every
implementation returns the correct sum, 499999500000.

| Language | ns per concurrency unit | Range |
|---|---:|---|
| Pony | 501 | 424-562 |
| Go | 562 | 502-663 |
| Aether | 594 | 417-676 |
| Erlang | 1,034 | 944-1200 |
| Elixir | 1,071 | 975-1204 |
| Scala | 6,561 | 5565-8794 |
| C | 9,230 | 8771-10087 |
| Rust | 14,763 | 13276-22556 |
| Zig | 15,272 | 14384-18666 |
| Java | 15,892 | 11878-20299 |
| C++ | 23,845 | 8666-36108 |

Read that as two groups, not eleven positions.

Pony, Go, Aether, Erlang and Elixir schedule their own units and pay half a
microsecond to one microsecond each. Scala, C, Rust, Zig, Java and C++ hand the
work to an OS thread or a thread pool and pay six to twenty-four.

**Within each group the ordering is not a result.** Pony, Go and Aether are 501,
562 and 594 with ranges that overlap almost entirely, so the suite does not show
any of them ahead of the others. The same applies to the thread-based group,
where these figures were taken on a machine that was also running several other
toolchains: C++ ranged from 8,666 to 36,108 across five runs, which is wider
than its distance from C. Run them on a quiet machine before quoting any pair.

An earlier pass reported single runs, which said Aether 471 against Go 799 and
read as a 1.7x win for Aether. It was noise, and rule 6 exists because of it.

### Scala depended on Akka, which broke rule 1

All five Scala implementations imported `akka.actor`, and `scala/build.sbt`
pulled `akka-actor` 2.8.5, so the Scala column measured a third-party framework
while the other ten measured standard libraries.

They now use `java.util.concurrent` from Scala: a bounded queue per mailbox for
counting, ping-pong, thread-ring and fork-join, and `ForkJoinPool` for skynet,
which is what the Java column uses. `build.sbt` has no dependencies at all.

This is the honest reading of base Scala for this suite. `scala.actors` was
removed in 2.13, so the language's own library has no actor system, and
`scala.concurrent.Channel` is deprecated. Dropping Akka made skynet **8x
faster**, 38,322 ns per unit down to 4,869 on the run that measured both, which
is itself a reason the column was misleading.

### Pony implemented three of five patterns

`pony/` had `counting`, `fork_join` and `thread_ring`, with no `ping_pong` and no
`skynet`, so the README's claim of 55 benchmarks with zero skips was really 53
and the runner would have failed on the two missing directories.

Both are now written and building. Pony's skynet is the fastest of the eleven at
501 ns per unit, which is worth having in the table: it is the other language
here whose actors are part of the language rather than a library.

### Git could not see Pony's benchmarks

The two missing patterns were partly a tooling problem. `.gitignore` removes the
compiled binaries with `benchmarks/cross-language/*/<pattern>`, and Pony compiles
a directory rather than a file, so each of those patterns also matched a Pony
source directory. A newly written Pony benchmark did not appear in `git status`
at all. The three that existed were committed before the rule and stayed
tracked, which is the only reason the gap looked like three of five rather than
zero of five.

Fixed in `.gitignore` by re-including `pony/*/` and ignoring its contents except
`*.pony`, so the binaries stay out and any future benchmark is visible.

### Ping-pong timed different regions

Aether, Erlang and Elixir created their units before starting the clock; C, C++,
Go, Rust, Zig, Java and Scala started the clock first. All eleven now start the
clock first, so unit creation is inside the measured region everywhere. Two units
against a million messages is well under a percent either way, but the rule is
that the region matches, not that the error is small.

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
  eleven must print the same N for the same leaf count.
- **Correctness.** Every implementation prints `Sum:`, which must be
  `499999500000` for 1,000,000 leaves. A wrong sum means work was skipped.
- **Dependencies.** `rust/Cargo.toml` and `scala/build.sbt` declare none, and
  the Go files import only the standard library.
- **Spread.** Run each at least five times. If the gap between two languages is
  smaller than either one's run-to-run range, the suite has not shown a
  difference between them.
