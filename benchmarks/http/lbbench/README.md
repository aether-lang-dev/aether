# Load-balancer comparison bench

Measures `std.http.server.lb` against nginx and haproxy, on one box, in one
run, with an optional A/B against another commit of this repo.

```sh
docker build -t aether-lbbench benchmarks/http/lbbench
docker run --rm --cpus=8 -v "$PWD":/src:ro aether-lbbench

# A/B the working tree against a commit, alternating them every round:
docker run --rm --cpus=8 -e AB_REF=origin/main -e ROUNDS=6 \
  -v "$PWD":/src:ro aether-lbbench
```

`DURATION`, `WARMUP`, `CONNECTIONS`, `THREADS`, `ROUNDS`, `GEN_CPUS`, `LB_CPUS`
and `AB_REF` are all environment variables.

## What it reports, and why

**rps**, and **CPU microseconds the balancer burned per request it served**,
read from the process's own `/proc` task stats. On a shared machine rps
measures everything running; CPU per request measures the work the code does,
and it is far steadier. Use it to judge a change, and rps to judge the result.

The nginx CPU figure reads 0 because nginx forks workers and the measurement
follows the master pid. aether and haproxy are single processes, so theirs are
real. Fixing that means summing the worker tree; nobody has needed it yet.

## Three things it does deliberately

**It measures nginx and haproxy in the same run.** A number from a run whose
controls moved is not a result. The summary prints nginx's spread across
rounds and says outright when a delta should not be read.

**It alternates the order every round.** Measuring A before B every time hands
any drift inside a round — the box warming, the page cache filling — to
whichever runs second, every time. The first version of this harness did that
and reported the subject as 17.8% slower; alternating the order brought the
same comparison to 0.9%. Almost all of the "regression" was the running order.

**It refuses to report a balancer that is not proxying.** A balancer answering
errors quickly reads as fast. A run against dead backends once showed +26%.

## What it has established

Replacing the ~30 per-request allocations of request parsing with a bump
arena moved throughput by −0.9% and CPU per request by +3.4%: no benefit.
Allocation *count* is not what limits this path, which is worth knowing before
anyone spends more effort there. See #1739.
