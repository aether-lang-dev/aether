# Fairness rules, and the audits that produced them

A cross-language benchmark is worth nothing if the implementations are not doing
the same work. This file records the rules the suite holds itself to and the
audit findings that led to them, so a future change can be checked against
something written down rather than against nobody's memory.

Each language is measured at a fair baseline: what its own standard library
provides, no external frameworks, written with the techniques an expert in it
would use, so that the suite adds no unfairness of its own.

## Rules

1. **Standard library only.** Each implementation uses what ships with the
   language: pthreads and `<stdatomic.h>` for C, `std::thread` and
   `std::atomic` for C++, goroutines and channels for Go, `java.lang.Thread`
   (platform and virtual) and `java.util.concurrent` for Java and Scala, `std`
   for Rust and Zig, OTP processes for Erlang and Elixir, language actors for
   Pony and Aether. No third-party actor framework, thread pool or allocator,
   and no exceptions.
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
7. **Each language is built by its own build file, at its release level, for
   the CPU it runs on.** The runner passes no compiler flags: it runs
   `make <pattern>` in the language's directory, and that Makefile calls the
   language's own tool. C, C++ and Aether compile with `-O3 -march=native
   -flto`; Rust with its release profile (`lto = "fat"`, `codegen-units = 1`)
   and `-C target-cpu=native` from `rust/.cargo/config.toml`; Zig with
   `ReleaseFast` for the native target, `build.zig`'s default; Go with
   `go build`, which optimises and links the whole program by default (its
   instruction-set level, `GOAMD64`, stays at the default: `v3` measured no
   different); Pony with `ponyc`'s release build, whole program, for the host
   CPU. Java, Scala, Erlang and Elixir compile to bytecode, and their JITs
   compile it for the host CPU. Each language's `make flags` is recorded with
   its results.
8. **Every result is checked.** Each implementation prints a `Checksum:` the
   runner knows in advance (ping-pong: the sum of the values ping got back;
   counting: the count; thread ring: the receipts summed over the 100 nodes;
   fork-join: the sum of the values the workers received; skynet: the sum),
   and skynet its `concurrency units:`. A run with a wrong value, a non-zero
   exit or no rate is `[FAIL]` and is not measured.
9. **The same warmup for every language.** One discarded run by default
   (`BENCH_WARMUP`), whether the language has a JIT or not.
10. **The technique an expert would use, fixed by the shape of the work, the
    same in every pattern of a language.** The shape is how many units there
    are against the cores (2, 1 and 8 units fit 24 cores; the ring's 100
    threads and skynet's 1,111 do not) and that every mailbox here has one
    sender at a time. The rule is fixed before measuring, never picked pattern
    by pattern from the results: Aether gets no such choice either. The
    section below gives each language's, and the audit lists every
    alternative measured.
11. **Say how each implementation waits, and report CPU time.** A runtime that
    keeps cores spinning while it waits gets a wall-clock rate from cores it
    occupies. Each language's wait strategy is written down below, and the
    results carry each run's CPU time and the messages per CPU-second beside
    the wall-clock rate, so that cost is visible rather than hidden.
12. **Rank like with like.** A language is ranked within its tier (below),
    never in one list of all eleven.

## Two tiers

Rules 1 and 10 give each language its standard library and its expert's
technique, and that makes two kinds of entry, which do different work for
the same message count:

- **Runtimes**: Aether, Go, Erlang, Elixir and Pony schedule their own units
  and own their mailboxes. A mailbox takes any sender and has no bound (Go's
  channels are bounded, 1,024 here), a unit can move between cores, and a
  failing one is isolated. That is the work a message costs in them.
- **Threads + queue**: C, C++, Rust and Zig have no actors in their standard
  libraries, so each unit is an OS thread and each mailbox the suite's
  bounded single-producer, single-consumer ring (`c/mailbox.h` and its
  ports): two atomic operations a message, a sender that waits when 1,024 are
  queued, nothing else. Java and Scala are OS threads with
  `java.util.concurrent` queues.

On counting the difference is the whole result: one sender, one receiver and
an increment, so the row measures the hand-off itself, and a two-thread ring
is close to the least that can cost (about 4 ns a message on an Apple M5
Pro, against 15 for the fastest runtime). Ranked in one list, a runtime is
read as losing to Rust's actors, which Rust's standard library does not have.

So the dashboard ranks within each tier, and the Relative column and the
summary are against the fastest of the row's tier (`lang_tier` in
`run_benchmarks.ae` writes each language's tier into the results). The
threads tier stays on the page as the floor the runtimes are read against,
not as their competition. The rules above hold in both tiers unchanged.

## Techniques

| Language | Concurrency units | Mailbox | How a receiver waits |
|---|---|---|---|
| Aether | actors on the runtime's scheduler, one thread per core | the runtime's | the scheduler spins about 10,000 idle passes with the CPU's pause hint, then parks (`runtime/scheduler/multicore_scheduler.c`) |
| C | pthreads | `c/mailbox.h`: a ring of sequence-numbered slots on C11 atomics | spins up to 1,000 pauses, then sleeps on a condition variable; the ring sleeps at once; skynet joins |
| C++ | `std::thread` | `cpp/mailbox.hpp`, the same ring on `std::atomic` | as C, on `std::condition_variable` |
| Rust | `std::thread` (scoped) | `rust/src/mailbox.rs`, the same ring on `std` atomics | as C, on `std::sync::Condvar` |
| Zig | `std.Thread` | `zig/mailbox.zig`, the same ring on `std.atomic` | as C, on the futex `std.Io` provides |
| Go | goroutines | buffered channels | the runtime parks the goroutine; an idle processor spins briefly looking for work before its thread sleeps |
| Java | platform threads, virtual threads for the ring, `ForkJoinPool` tasks for skynet | `LinkedTransferQueue`; `ArrayBlockingQueue` for the ring | `LinkedTransferQueue` spins briefly, then parks; `ArrayBlockingQueue` parks at once |
| Scala | as Java, through the same `java.util.concurrent` | as Java | as Java |
| Erlang, Elixir | processes | the process mailbox | the BEAM's schedulers busy-wait before they sleep (`+sbwt`, default `medium`) |
| Pony | actors | the actor's queue | the scheduler threads spin, then suspend |

The rule for each:

- **C, C++, Rust and Zig** create an OS thread per unit, so they share one
  rule. Every mailbox in these patterns has one sender at a time, and the
  expert's tool for that shape is a single-producer, single-consumer ring on
  the standard library's atomics. Each uses the same one, a sequence number
  in every slot (Vyukov's bounded queue for one sender and one receiver), so
  neither side reads the other's index and a message moves the cache line of
  its slot and nothing else. Where the units fit the cores (ping-pong,
  counting, fork-join) a receiver spins up to 1,000 pauses, looking again
  after 1, 2, 4, ... 64 of them, then sleeps: the same strategy the actor
  runtimes use. Where they do not (the 100-thread ring) it sleeps at once,
  because a hundred spinning threads take the cores from the one that holds
  the token; skynet's 1,111 threads join their children, which blocks at
  once too. C and C++ have no channel in their standard libraries. Rust's
  `std::sync::mpsc` and Zig's `std.Io.Queue` are general queues, and the
  audit below shows what they measured.
- **Java and Scala**: platform threads where the units fit the cores, with
  `LinkedTransferQueue`, which spins briefly before it parks; virtual threads
  where they do not (the ring), with `ArrayBlockingQueue`, which parks at
  once; skynet's recursive split is what `ForkJoinPool` and `RecursiveTask`
  are for. Virtual threads need Java 21.
- **Go, Erlang, Elixir, Pony and Aether** schedule their own units and own
  their mailboxes; there is no choice to make, and each runtime spins before
  it parks.
- **Mailbox size**: 1,024 messages for counting and fork-join wherever the
  language bounds a mailbox (C, C++, Rust, Zig, Go), one where one message is
  in flight at a time (ping-pong, the ring). `LinkedTransferQueue` and the
  actor runtimes' mailboxes are unbounded.

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

## Audit, October 2026

Issue #3076 listed what `make benchmark` showed: a median that was not one,
results nobody checked, C and C++ sleeping and waking on every message while
Aether's scheduler spins, build flags that differed by language, a warmup only
the JVM and BEAM got, Zig not building, and memory reading 5 MB for every
language on Windows. Rules 7 to 11 come from it.

### How it was measured

An i7-13700K (8 performance and 8 efficiency cores, 24 logical), Windows 11,
GCC 16.2 (MSYS2 MinGW), rustc 1.92, Go 1.25.5, Zig 0.16.0 and Temurin JDK
21.0.12.1. The editor's indexer kept about one core of the 24 busy throughout.
One million messages; one warmup run and five timed runs of every variant,
the rounds interleaved (each round runs every variant once, so drift lands on
all of them alike); the median and the range, in millions of messages per
second; CPU time from the process handle, which sees every thread. Three
batches: an anchor repeated across them agreed within its range (the C ring
of sequence-numbered slots: 54.62 in counting in the second, 58.56 in the
third).

Erlang, Elixir, Pony and Scala are not installed on this machine. Their
changes were reviewed by reading, Scala's choices follow Java's measured
ones, and the CI job (`.github/workflows/benchmarks.yml`) builds and checks
all eleven languages.

### The runner

- **The median was the middle run in time order.** The runner kept the run at
  index `runs / 2` as the runs happened: Aether ping-pong's 12.34, 12.29,
  12.58, 12.09, 12.62 reported 12.58 where the median is 12.34, 1.9% high.
  Its "CV" was range / mean * 50, not a coefficient of variation.
- **No result was checked.** Every implementation now prints a checksum the
  runner verifies (rule 8). The checksums found the errors in the next
  section.
- **Only the JVM and BEAM languages got a warmup.** Every language gets the
  same one now (rule 9). The native languages' first runs fell inside their
  ranges, except Aether's ping-pong, 10.71 against a median of 12.10.
- **C++ was built by the runner** with `g++ -O3 -std=c++17 -march=native`,
  not by its Makefile's `-flto -fno-plt`; Rust built without
  `target-cpu=native`; Scala ran under `sbt runMain`, so its measurement held
  sbt's own JVM. Each is built by its own build file now (rule 7). The flag
  differences made no difference that five runs can see:

  | Build | ping-pong | counting | thread ring | fork-join | skynet |
  |---|---|---|---|---|---|
  | C++ without -flto | 0.20 (0.20-0.22) | 16.90 (16.44-18.70) | 0.36 (0.35-0.36) | 0.79 (0.79-0.80) | 0.04 |
  | C++ with it | 0.20 (0.20-0.22) | 17.90 (14.61-19.16) | 0.36 (0.35-0.36) | 0.79 (0.78-0.81) | 0.04 |
  | Rust without native | 1.86 (1.77-2.03) | 21.42 (18.45-62.53) | 0.33 (0.33-0.34) | 1.29 (1.25-1.36) | 0.04 |
  | Rust with it | 2.02 (1.63-2.26) | 23.00 (17.42-27.33) | 0.34 (0.33-0.34) | 1.24 (1.16-1.56) | 0.04 |
  | Go, default GOAMD64 | 2.84 (2.70-2.95) | 31.26 (30.47-32.32) | 5.28 (5.27-5.32) | 18.36 (17.56-19.42) | 1.08 |
  | Go, GOAMD64=v3 | 2.92 (2.75-2.95) | 31.85 (30.09-32.97) | 5.33 (5.20-5.38) | 18.33 (17.97-19.35) | 1.08 |

  (The C++ and Rust rows are the code from before the wait-strategy changes
  below; the rule is that the build files decide, whatever the result.)
- **Memory read 5 MB for every language on Windows**, and CPU time 0 s: MSYS's
  `time` sees only itself, not a native child. Both are n/a there now, and
  measured with `/usr/bin/time` on Linux and macOS.
- **Toolchain versions and flags were not recorded.** They are in the results
  JSON and on the page.

### Errors the checksums found

- **Scala's fork-join sent the wrong values.** Round i sent the value i to
  every worker instead of i * 8 + k to worker k: the same number of messages,
  so the rate was right and the checksum was not.
- **Rust and Pony sent N fork-join messages**, not the 8 * (N / 8) every other
  language sends: the same at a million, which 8 divides, and a different
  count of work at any other N.
- **Scala's thread ring overran.** After the token reached 0 it went on round
  the ring to the last node, up to 99 messages the rate did not count, and the
  timed region included joining the 100 threads.

### Wait strategies

C and C++ handed every message over with a mutex and a condition variable,
so each one cost a sleep and a wake-up: 0.20 M round trips a second in
ping-pong, 3.7 s of CPU for a million, while Aether did 12.10 on 0.20 s.
Throughput alone credits a runtime for spinning, and these two never spun.
The rule above fixed each language's technique, and these are all the
alternatives measured. M msg/s, median (range); the chosen row is in bold.

C:

| Mailbox | ping-pong | counting | thread ring | fork-join |
|---|---|---|---|---|
| mutex + condition variable (before) | 0.20 (0.20-0.21) | 15.09 (13.26-16.16) | 0.38 (0.38-0.38) | 0.81 (0.79-0.83) |
| ring of indices, spin 100 | 7.12 (6.67-7.18) | 20.38 (18.07-21.02) | 0.31 (0.30-0.31) | 8.88 (8.70-10.06) |
| ring of indices, spin 1,000 | 7.06 (6.98-7.23) | 20.46 (19.59-21.11) | 0.19 (0.19-0.19) | 22.85 (20.94-23.93) |
| ring of indices, spin 10,000 | 6.80 (6.42-7.28) | 20.41 (19.88-21.40) | 0.09 (0.08-0.09) | 22.11 (20.98-23.07) |
| ring of indices, backed-off spin | 5.18 (4.59-5.52) | 26.16 (24.35-27.54) | 0.38 (0.37-0.38), spin 0 | 27.29 (23.43-28.90) |
| **sequence-numbered slots, backed-off spin; ring: spin 0** | **7.50 (7.31-7.71)** | **57.23 (53.24-65.19)** | **0.38 (0.37-0.38)** | **33.62 (33.29-35.38)** |

The thread ring is the oversubscribed case: spinning 100, 1,000 and 10,000
pauses cost it 5.7, 31.9 and 243 s of CPU per run against 2.0 s for sleeping
at once, and made it slower. In the other patterns the spin bound mattered
only below 1,000 (fork-join at 100: 8.88). The ring of indices made the
receiver read the sender's index, and each look moved that cache line back
and forth; backing off between looks helped counting and fork-join and cost
ping-pong its latency; slots that carry their own sequence number removed
the shared index and won or tied everywhere.

C++:

| Mailbox | ping-pong | counting | thread ring | fork-join |
|---|---|---|---|---|
| mutex + condition variable (before) | 0.20 (0.20-0.22) | 17.90 (14.61-19.16) | 0.36 (0.35-0.36) | 0.79 (0.78-0.81) |
| ring of indices, C++20 `atomic::wait` | 5.52 (5.07-5.68) | 19.86 (18.90-24.38) | 0.12 (0.12-0.12) | 20.94 (20.48-21.34) |
| ring of indices, condition variable | 5.89 (5.73-6.22) | 19.65 (18.28-22.46) | 0.19 (0.19-0.19) | 22.98 (21.88-23.94) |
| ring of indices, backed-off spin | 3.73 (3.65-3.76) | 25.62 (22.91-26.71) | 0.37 (0.37-0.38), spin 0 | 29.14 (26.22-30.39) |
| **sequence-numbered slots, as C** | **7.21 (7.18-7.40)** | **94.11 (91.41-110.34)** | **0.38 (0.38-0.39)** | **32.37 (31.40-32.66)** |

C++20's `std::atomic::wait` lost to the condition variable in every pattern
(libstdc++ on MinGW, which waits through a condition-variable pool; Linux's
futex path may differ), and spent 137 s of CPU on the ring.

Rust:

| Mailbox | ping-pong | counting | thread ring | fork-join |
|---|---|---|---|---|
| before: `sync_channel(1)`; `channel()` for the rest | 2.02 (1.63-2.26) | 23.00 (17.42-27.33) | 0.34 (0.33-0.34) | 1.24 (1.16-1.56) |
| `std::sync::mpsc::channel()` | 1.52 (1.41-1.95) | 22.77 (11.94-60.10) | 0.32 (0.32-0.34) | 1.18 (1.08-1.30) |
| `std::sync::mpsc::sync_channel(1 / 1,024)` | 1.85 (1.56-2.10) | 89.90 (72.67-105.64) | 0.32 (0.31-0.34) | 2.34 (2.00-3.86) |
| ring of indices, spin 1,000 | 6.07 (5.83-6.11) | 23.95 (23.21-27.14) | 0.20 (0.20-0.20) | 22.10 (20.41-22.38) |
| ring of indices, backed-off spin | 3.35 (3.30-3.38) | 27.59 (27.20-29.54) | 0.32 (0.32-0.33), spin 0 | 29.34 (28.00-30.85) |
| **sequence-numbered slots, as C** | **7.77 (7.44-14.55)** | **54.65 (51.78-59.46)** | **0.34 (0.32-0.34)** | **35.52 (33.81-36.14)** |

`sync_channel` leads counting (90.94 in the third batch, beside the chosen
54.65), where one sender streams into a bounded buffer; it is a quarter of
the ring's speed in ping-pong and a tenth or less in fork-join, where
receivers wait often. The rule takes one mailbox for the shape, not the faster per pattern.
Skynet joins its children now, as C, C++ and Zig do, instead of collecting
their sums over a channel that spins and yields before it sleeps: 0.04 M
units a second either way.

Zig (before: did not build):

| Mailbox | ping-pong | counting | thread ring | fork-join |
|---|---|---|---|---|
| `std.Io.Queue` | 0.21 (0.20-0.21) | 21.01 (20.37-21.31) | 0.40 (0.40-0.42) | 0.89 (0.88-0.90) |
| ring of indices, spin 1,000 | 3.91 (3.61-4.38) | 9.90 (9.30-10.00) | 0.21 (0.20-0.21) | 14.32 (13.42-14.61) |
| ring of indices, backed-off spin | 2.46 (2.45-2.51) | 11.94 (11.19-13.55) | 0.41 (0.40-0.41), spin 0 | 20.85 (20.50-21.14) |
| **sequence-numbered slots, as C** | **7.34 (7.16-7.71)** | **58.09 (51.79-62.10)** | **0.42 (0.40-0.44)** | **34.57 (31.15-35.22)** |

`std.Io.Queue` puts every message under a mutex and sleeps at once, which
suits the oversubscribed ring (0.43 in the third batch, beside 0.42) and
nothing else.

Java (Scala takes the same choices):

| Threads and queue | ping-pong | counting | thread ring | fork-join |
|---|---|---|---|---|
| platform, `ArrayBlockingQueue` (before) | 0.19 (0.18-0.20) | 9.23 (8.76-9.66) | 0.36 (0.33-0.36) | 0.82 (0.81-0.87) |
| platform, `LinkedTransferQueue` | 3.46 (3.38-3.49) | 9.88 (8.91-10.92) | 0.25 (0.24-0.25) | 10.03 (9.81-10.55) |
| platform, `LinkedBlockingQueue` | 0.18 (0.18-0.19) | 6.38 (6.16-7.72) | 0.36 (0.34-0.36) | 0.79 (0.79-0.83) |
| virtual, `ArrayBlockingQueue` | 0.98 (0.92-0.99) | 8.83 (7.59-9.16) | 1.77 (1.71-1.81) | 4.19 (4.03-4.48) |
| virtual, `LinkedTransferQueue` | 1.06 (1.04-1.09) | 11.87 (10.68-12.20) | 1.66 (1.61-1.73) | 6.72 (5.70-14.62) |
| **platform + `LinkedTransferQueue`; ring: virtual + `ArrayBlockingQueue`** | **3.37 (3.20-3.44)** | **9.51 (8.94-10.08)** | **1.74 (1.67-1.79)** | **10.60 (9.37-11.49)** |

Virtual threads won counting by a fifth, but one consumer fits the cores, so
the rule keeps it on a platform thread; nothing in the shape of counting
explains that win.

Go has no choice of technique. A one-slot buffer, the mailbox of the other
languages, measured 2.84 (2.70-2.95) in ping-pong and an unbuffered channel
2.87 (2.77-2.93); 16,384-slot mailboxes measured 33.35 (31.93-34.50) in
counting and 17.27 (15.52-17.88) in fork-join against 1,024 slots' 31.26 and
18.36. Go keeps 1 and 1,024.

### Found reading the code

- **The timed region (rule 5).** Counting, the thread ring and fork-join
  created their units before starting the clock in Aether, C, C++, Go, Java,
  Zig, Erlang and Elixir, and so did Pony's ring and fork-join; Scala started
  the clock first. All of them start it first now. Java's skynet also
  created its pool before the clock.
- **Aether's fork-join picked the worker with a division and eight
  comparisons per message,** sequential work no other language did; it
  indexes an array with `i % 8` now.
- **Aether's skynet sent every unit two messages**, a `Setup` telling it its
  own reference and then its work, where every other language sends one; a
  node passes `self` as the parent now.
- **Elixir's counting sent with `Enum.each`**, an anonymous function call per
  message that its own fork-join and Erlang do not make; it uses a recursive
  loop now.
- **Java and Scala printed their rates in the default locale**, so where it
  uses a decimal comma they printed `2,38` and the runner read no rate. They
  print with `Locale.ROOT` now.
- **Java's thread ring gave each node an 8-message mailbox** for its one
  token; one now, as everywhere else. Java's and Scala's skynet printed no
  rate for a run under a microsecond, as skynet elsewhere once did.
- **Zig's skynet joined an undefined thread** when a spawn failed, having
  summed that subtree on the parent instead; a failed spawn is a failed run
  now.

### Zig did not build

Zig 0.16 removed `std.Thread.Mutex`, `std.Thread.Condition`,
`std.posix.getenv` and `std.heap.GeneralPurposeAllocator`, and every Zig
benchmark failed to compile, so the suite had been reporting ten languages.
They are ported to 0.16: `main` takes `std.process.Init`, waiting goes
through the futex of `std.Io`, and the output goes to stdout instead of
`std.debug.print`'s stderr. Zig's `fork_join.zig` also took each message off
its queue with `orderedRemove(0)`, which moves every message behind it; the
mailbox above replaced it.

### Aether is bimodal on this machine

Aether's ping-pong and counting measured steady (12.10, range 11.79-12.26;
42.86, range 40.20-43.04). Its thread ring and fork-join did not: fork-join
ran 1.53 to 21.51 within five runs, and the ring 0.09 to 0.78, with a
warmup run of 3.71. The changes above to Aether's fork-join and skynet show
up in those numbers as noise (fork-join 19.33 before, 6.35 after; skynet 0.33
before, 0.75 after) and no conclusion is drawn from them. The cause is in the
scheduler on a hybrid CPU, not in the benchmarks: aether#3087.

### Where the numbers stand

The languages measured here, with the CPU time each run took. Aether and Go
from the first batch, the others from the third.

| Language | ping-pong | CPU s | counting | thread ring | CPU s | fork-join |
|---|---|---|---|---|---|---|
| Aether | 12.10 (11.79-12.26) | 0.20 | 42.86 (40.20-43.04) | 0.34 (0.09-0.78) | 7.89 | bimodal, above |
| C | 7.50 (7.31-7.71) | 0.27 | 57.23 (53.24-65.19) | 0.38 (0.37-0.38) | 2.09 | 33.62 (33.29-35.38) |
| C++ | 7.21 (7.18-7.40) | 0.28 | 94.11 (91.41-110.34) | 0.38 (0.38-0.39) | 2.09 | 32.37 (31.40-32.66) |
| Rust | 7.77 (7.44-14.55) | 0.23 | 54.65 (51.78-59.46) | 0.34 (0.32-0.34) | 5.02 | 35.52 (33.81-36.14) |
| Zig | 7.34 (7.16-7.71) | 0.28 | 58.09 (51.79-62.10) | 0.42 (0.40-0.44) | 1.67 | 34.57 (31.15-35.22) |
| Go | 2.84 (2.70-2.95) | 0.56 | 31.26 (30.47-32.32) | 5.28 (5.27-5.32) | 0.28 | 18.36 (17.56-19.42) |
| Java | 3.37 (3.20-3.44) | 0.69 | 9.51 (8.94-10.08) | 1.74 (1.67-1.79) | 2.00 | 10.60 (9.37-11.49) |

Read it as before: ranges first. The OS-thread languages now sit together in
every pattern, as they should with one technique between them. Aether's
ping-pong lead over them comes with less CPU per round trip (0.20 s for a
million against their 0.23 to 0.28 s), so spinning does not buy it. The ring
belongs to the runtimes that schedule their own units: Go, Java's virtual
threads, and Aether when it is in its fast mode.

## Checking a change against these rules

- **Results.** The runner checks every run's `Checksum:` and skynet's
  `concurrency units:` (rule 8), and `BENCH_STRICT=1` fails the run when any
  selected language does not build or is wrong. Skynet's sum for 1,000,000
  leaves is `499999500000` in every language, and each prints 1,111 units.
- **Every language, every pattern.** The CI job
  (`.github/workflows/benchmarks.yml`) builds all eleven and runs each
  pattern with a small count on a change here and once a week; its summary
  names any language and pattern that broke.
- **Technique.** A change to how a language waits or queues follows its rule
  above for every pattern, with the alternatives it measured added here.
- **Dependencies.** `rust/Cargo.toml` and `scala/build.sbt` declare none, and
  the Go files import only the standard library.
- **Spread.** Run each at least five times. If the gap between two languages is
  smaller than either one's run-to-run range, the suite has not shown a
  difference between them.
