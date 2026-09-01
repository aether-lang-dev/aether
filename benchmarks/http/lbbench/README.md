# Load-balancer comparison bench

Measures `std.http.server.lb` against nginx and haproxy on one box, in one run,
with an optional A/B against another commit of this repo.

```sh
docker build -t aether-lbbench benchmarks/http/lbbench
docker run --rm --cpus=8 -v "$PWD":/src:ro aether-lbbench

# A/B the working tree against a commit, alternating them every round:
docker run --rm --cpus=8 -e AB_REF=origin/main -e ROUNDS=6 \
  -v "$PWD":/src:ro aether-lbbench
```

`DURATION`, `WARMUP`, `CONNECTIONS`, `THREADS`, `ROUNDS`, `GEN_CPUS`, `LB_CPUS`
and `AB_REF` are environment variables.

The point is the ratio, not the absolute numbers. All three balancers proxy to
the same backends through the same generator in one session, so box speed and
container overhead cancel. A number from this harness is comparable only to
another number from the same run.

## The instruments

All three balancers are configured to do the same job, which took a correction:
the aether one adds `X-Forwarded-For`, `X-Forwarded-Proto`, `X-Forwarded-Host`
and `Via` to every forwarded request and the controls originally added none of
them. A comparison where one side builds four headers and the others forward
as-is measures the configuration, not the code. The controls now add the same
four.

| script | measures | use it when |
|---|---|---|
| `run.sh` | rps and CPU per request, with controls | judging a result |
| `profile.sh` | where the balancer's CPU goes | choosing what to change |
| `syscalls.sh` | syscalls per request, exactly | checking a syscall change |
| `switches.sh` | which call asked to sleep, by name | chasing context switches |
| `instructions.sh` | work done per request, in instructions | pricing a change in userspace work |
| `split.sh` | user vs kernel cycles per request, every subject | deciding which half the gap is in |
| `cachemisses.sh` | cache misses per request, aether and the controls | when fewer instructions are not faster |

Reach for `split.sh` when the counters disagree about where a gap can be:
the same syscalls per request as nginx and twice the CPU means the extra is
either in the calls or before them, and that decides what to change. It counts
rather than samples, so the split survives a noisy box even though the
absolute cycles per request do not.

`profile.sh`, `switches.sh`, `instructions.sh` and `split.sh` need `perf`; `switches.sh`
needs a privileged container for the scheduler tracepoint, and
`instructions.sh` needs a hardware performance counter, which a virtual
machine usually does not expose. It says so and stops rather than reporting a
count it could not take.

### Fewer instructions is not the same as faster

`cachemisses.sh` exists because an instruction count sent an optimisation
effort in the wrong direction. aether executes **fewer** instructions per
request than nginx and is still slower per core:

| per request, 50 connections | instructions | L1 data misses | last-level misses |
|---|---|---|---|
| aether | 18,469 | 185 | **90** |
| nginx | 23,743 | 492 | **12** |

An L1 miss is answered by L2 or L3 and costs tens of cycles. A last-level miss
goes to memory and costs hundreds. Nearly all of nginx's references are
answered by cache; ninety of ours per request are not, and at roughly 80ns
each that is several microseconds, which is the whole of the gap. Work on this
path should be judged on that column, not on instructions.

Two things about measuring it, both learned by getting them wrong first:

- **Concurrency is not optional.** Driving a single connection shows aether
  ahead of nginx on every cache metric. The misses only appear when many
  connections take turns and each one's memory has gone cold in between, which
  is what the real benchmark does and what a one-connection probe cannot see.
- **Buffer size is not working set.** Shrinking the per-connection buffers
  from 16 KiB to 4 KiB moved this measurement by nothing, because the pages
  past what a request actually touches were never read or written. Only
  touched bytes are working set, and that change was reverted.

Reach for `instructions.sh` when the question is whether a change made the
code do more work, rather than how fast it ran: CPU per request moves with
everything else on the box, and on a loaded machine its medians have
disagreed by more than 20% between two builds whose least-contended rounds
differed by 1.5%. Instructions say nothing about stalls or time spent asleep,
which is where most of this path's cost is, so it prices a change but does not
judge a result.

## What `run.sh` reports

**rps**, and **CPU microseconds the balancer burned per request it served**,
read from the process's own `/proc` entry and summed over its whole process
tree. On a shared machine rps measures everything running; CPU per request
measures the work the code does, and it is far steadier.

**Context switches per request**, split into voluntary and involuntary.
Voluntary means the code asked to sleep. Involuntary means the scheduler took
the CPU away, which is threads oversubscribing the cores. The two call for
opposite fixes, so they are never summed into one figure.

## Guarantees it makes about its own numbers

**Both controls are measured in the same run.** A number from a run whose
controls moved is not a result, so the summary prints nginx's spread across
rounds and says outright when a delta should not be read.

**The order alternates every round.** Measuring A before B every time hands any
drift inside a round, the box warming or the page cache filling, to whichever
runs second.

**A balancer that is not proxying is not reported.** One answering errors
quickly reads as fast, so every subject is verified to return a backend
response before it is measured.

**A figure it could not measure is reported as `cpu UNMEASURED`, never as 0.**
Zero is the most flattering number in the table and is never true of a process
that served requests, so it is never printed as one. Rounds that could not be
measured keep their rps and are excluded from the CPU median, and the count of
skipped rounds is shown.

**Editing an instrument takes effect without rebuilding the image.** The
scripts are baked in, so each re-execs from the mounted tree when it differs,
and says so, rather than silently measuring with the version in the image.

Throughput can be unresolvable on a shared or virtualised box, where controls
have moved over 100% between rounds. Syscall and context-switch counts do not
care, so when a change is about either, count them and say so rather than
quoting a throughput delta the box cannot support.

Results and findings live in the issue tracker, not here.
