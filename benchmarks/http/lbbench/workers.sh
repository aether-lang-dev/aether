#!/bin/bash
# Does throughput scale with the worker count?
#
# A worker owns its connection for the whole of a request, and a proxy's
# handler blocks for an upstream round trip, so in-flight requests cannot
# exceed the worker count. If that is the bound, throughput rises with workers
# and flattens when something else takes over. If it is flat from the start,
# the bound is elsewhere and the thread model is not what to change.
#
# This is a diagnosis, not a benchmark: it says which wall we are against.
set -uo pipefail

DURATION="${DURATION:-10s}"
CONNECTIONS="${CONNECTIONS:-50}"
SWEEP="${SWEEP:-8 16 32 64}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
done

say "building ..."
cp -r /src /build && cd /build && rm -rf build
make -j"$(nproc)" >/tmp/build.log 2>&1 || { tail -20 /tmp/build.log >&2; die "build failed"; }
./build/ae build benchmarks/http/lb_reuse_lb.ae -o /tmp/ae-lb >/tmp/lb.log 2>&1 || die "lb build failed"

say ""
printf '%8s %12s %10s\n' "workers" "rps" "p99" >&2
for w in $SWEEP; do
    pkill -f '/tmp/ae-lb' >/dev/null 2>&1; sleep 1
    AETHER_HTTP_WORKERS="$w" BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
        taskset -c 2,3 /tmp/ae-lb >/tmp/ae-lb.log 2>&1 &
    ok=0
    for _ in $(seq 1 40); do
        case "$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null)" in backend-*) ok=1; break ;; esac
        sleep 0.25
    done
    [ "$ok" = 1 ] || { printf '%8s %12s\n' "$w" "NOT PROXYING" >&2; continue; }
    taskset -c 0,1 wrk -t2 -c"$CONNECTIONS" -d4s http://127.0.0.1:18200/ >/dev/null 2>&1
    out=$(taskset -c 0,1 wrk -t2 -c"$CONNECTIONS" -d"$DURATION" --latency http://127.0.0.1:18200/ 2>&1)
    printf '%8s %12s %10s\n' "$w" \
        "$(printf '%s' "$out" | awk '/^Requests\/sec:/ {print $2}')" \
        "$(printf '%s' "$out" | awk '/99%/ {print $2; exit}')" >&2
done
pkill -f '/tmp/ae-lb' >/dev/null 2>&1

# nginx, once, for scale.
nginx -c /bench/nginx-lb.conf -p /tmp
taskset -c 0,1 wrk -t2 -c"$CONNECTIONS" -d4s http://127.0.0.1:18200/ >/dev/null 2>&1
out=$(taskset -c 0,1 wrk -t2 -c"$CONNECTIONS" -d"$DURATION" --latency http://127.0.0.1:18200/ 2>&1)
printf '%8s %12s %10s\n' "nginx" \
    "$(printf '%s' "$out" | awk '/^Requests\/sec:/ {print $2}')" \
    "$(printf '%s' "$out" | awk '/99%/ {print $2; exit}')" >&2
