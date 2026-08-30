#!/bin/bash
# Which allocations still fault a page in.
#
# Page faults per request are the last load-independent difference left
# against nginx: 0.05 against its 0.00, with the kernel zeroing those pages
# costing about 4% of the profile. A fault is not slow because a malloc is
# slow; it is the kernel handing back a page and taking it again. This says
# which allocation is doing that, the same way the trace that found the
# request arena did.
set -uo pipefail

. /bench/use_mounted.sh
lbbench_use_mounted faults.sh "$@"

DURATION="${DURATION:-15}"
CONNECTIONS="${CONNECTIONS:-50}"
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

PERF=$(ls /usr/lib/linux-tools-*/perf 2>/dev/null | head -1)
[ -n "$PERF" ] || PERF=$(command -v perf)
[ -n "$PERF" ] || die "perf not installed in the image"
sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1 || true

nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
done

say "building aether ..."
cp -r /src /build && cd /build && rm -rf build
make -j"$(nproc)" >/tmp/build.log 2>&1 || { tail -20 /tmp/build.log >&2; die "build failed"; }
./build/ae build benchmarks/http/lb_reuse_lb.ae -o /tmp/ae-lb >/tmp/lbbuild.log 2>&1 \
    || { tail -20 /tmp/lbbuild.log >&2; die "lb build failed"; }

BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
    taskset -c "$LB_CPUS" /tmp/ae-lb >/tmp/ae-lb.log 2>&1 &
LB=$!
for _ in $(seq 1 40); do
    body=$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null) && case "$body" in backend-*) break ;; esac
    sleep 0.25
done
curl -sf -m 5 http://127.0.0.1:18200/ >/dev/null || die "balancer is not proxying"

# Warm first: start-up faults are not the ones being looked for.
taskset -c "$GEN_CPUS" wrk -t2 -c"$CONNECTIONS" -d10s http://127.0.0.1:18200/ >/dev/null 2>&1

taskset -c "$GEN_CPUS" wrk -t2 -c"$CONNECTIONS" -d"${DURATION}s" \
    http://127.0.0.1:18200/ >/tmp/wrk.out 2>&1 &
WRK=$!
"$PERF" record -e page-faults -g --pid "$LB" -o /tmp/pf.data -- sleep "$DURATION" >/dev/null 2>&1
wait "$WRK" 2>/dev/null
kill "$LB" 2>/dev/null

awk '/Requests\/sec/ {print "  during the trace: " $2 " rps"}' /tmp/wrk.out >&2
say ""
say "== page faults, by the balancer's own frames =="
# The leaf is almost always inside libc and often has no symbol, which says
# nothing: what matters is which of our allocations led to it. --children
# attributes a fault to every frame on the way down, so our own function names
# appear even when the page was taken by memset inside malloc.
"$PERF" report -i /tmp/pf.data --stdio --children --percent-limit 1 --comms=ae-lb 2>/dev/null \
    | grep -E '^\s+[0-9]+\.[0-9]+%' | grep -E 'ae-lb\s+ae-lb|\[\.\] [a-z_]+' | head -20
say ""
say "== and the raw leaves, for completeness =="
"$PERF" report -i /tmp/pf.data --stdio --no-children --percent-limit 1 2>/dev/null \
    | grep -E '^\s+[0-9]+\.[0-9]+%' | head -8
