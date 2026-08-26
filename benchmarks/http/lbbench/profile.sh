#!/bin/bash
# Where does the aether load balancer's CPU actually go?
#
# Same setup as run.sh, two backends and a pinned generator and balancer, but
# instead of timing it, this samples the balancer under load and prints the
# hot paths. Guessing produced one change worth nothing (#1739); this is the
# instrument for choosing the next one.
#
# perf needs kernel.perf_event_paranoid <= 1 to sample another process. In a
# privileged container that can be set here; without privilege, user-space
# sampling of our own child still works at 2 on most kernels, and the script
# says which it got rather than silently profiling nothing.
set -uo pipefail

. /bench/use_mounted.sh
lbbench_use_mounted profile.sh "$@"

DURATION="${DURATION:-20}"
CONNECTIONS="${CONNECTIONS:-50}"
THREADS="${THREADS:-2}"
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"
FREQ="${FREQ:-997}"          # prime, so it does not beat with periodic work

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

PERF=$(ls /usr/lib/linux-tools-*/perf 2>/dev/null | head -1)
[ -n "$PERF" ] || PERF=$(command -v perf)
[ -n "$PERF" ] || die "perf not installed in the image"

sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1 \
    && say "perf_event_paranoid set to 1" \
    || say "could not lower perf_event_paranoid (now $(cat /proc/sys/kernel/perf_event_paranoid)); sampling may be user-space only"

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

# Warm, so start-up work does not colour the profile.
taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d5s http://127.0.0.1:18200/ >/dev/null 2>&1

say "sampling $DURATION s at ${FREQ}Hz under load ..."
taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" \
    http://127.0.0.1:18200/ >/tmp/wrk.out 2>&1 &
WRK=$!
"$PERF" record -F "$FREQ" -g --pid "$LB" -o /tmp/perf.data -- sleep "$DURATION" >/dev/null 2>&1
wait "$WRK" 2>/dev/null
kill "$LB" 2>/dev/null

awk '/Requests\/sec/ {print "  during the profile: " $2 " rps"}' /tmp/wrk.out >&2
say ""
say "== self time, top 30 =="
"$PERF" report -i /tmp/perf.data --stdio --no-children --percent-limit 0.3 2>/dev/null \
    | grep -E '^\s+[0-9]' | head -30
say ""
say "== callers of the heaviest leaf =="
"$PERF" report -i /tmp/perf.data --stdio --children --percent-limit 1 2>/dev/null \
    | grep -E '^\s+[0-9]' | head -20
