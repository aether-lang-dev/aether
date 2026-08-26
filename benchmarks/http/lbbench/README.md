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

**Context switches per request**, from the same source. A thread-per-request
server pays these where an event loop does not, so this is the number that
prices the thread model. All three figures sum the balancer's whole process
tree: nginx forks workers and does its work there, so following the master
alone reported 0 CPU and 0 context switches for it, a flattering number that
said nothing.

## Three things it does deliberately

**It measures nginx and haproxy in the same run.** A number from a run whose
controls moved is not a result. The summary prints nginx's spread across
rounds and says outright when a delta should not be read.

**It alternates the order every round.** Measuring A before B every time hands
any drift inside a round (the box warming, the page cache filling) to
whichever runs second, every time. The first version of this harness did that
and reported the subject as 17.8% slower; alternating the order brought the
same comparison to 0.9%. Almost all of the "regression" was the running order.

**It refuses to report a balancer that is not proxying.** A balancer answering
errors quickly reads as fast. A run against dead backends once showed +26%.

**It refuses to report a figure it could not measure.** A CPU or context
switch figure it cannot read is printed as `cpu UNMEASURED`, never as 0.
Zero is the most flattering number in the table and is never true of a
process that served requests. nginx daemonizes, so its master pid comes from
a pid file that is not written yet when the launcher returns; reading it
straight away got the previous round's dead pid and reported 0 for the whole
nginx column. Only nginx was affected, because aether runs in the foreground
and haproxy is found by pgrep, so findings that compare aether against a
baseline were never touched by it.

## Three instruments, and when to use which

| script | measures | use it when |
|---|---|---|
| `run.sh` | rps and CPU per request, with controls | judging a result |
| `profile.sh` | where the balancer's CPU goes | choosing what to change |
| `syscalls.sh` | syscalls per request, exactly | checking a syscall change |

Throughput can be unresolvable on a shared or virtualised box: a run here has
had its controls move 198% between rounds. Syscall counts do not care: they
are what the code asks the kernel to do. When a change is about syscalls,
count them and say so, rather than quoting a throughput delta the box cannot
support.

## What it has established

**Allocation count is not what limits this path.** Replacing the ~30
per-request allocations of request parsing with a bump arena moved throughput
by −0.9% and CPU per request by +3.4%: no benefit. `profile.sh` says the same
thing from the other side: `malloc` is 1.29% of self time, while syscall
entry is 67%. The arena was written, measured and dropped. See #1739.

**Syscalls are where the cost is.** Baseline measured 10.07 per proxied
request, against the ~5 that #1719 measured for nginx. Removing the poll that
preceded a read on a kept-alive connection took that to 9.24.
