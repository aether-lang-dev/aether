#!/bin/bash
# Which of the balancer's threads actually spend the CPU.
#
# split.sh reads CPU per request from /proc, and /proc sums every thread of the
# process. A server that runs threads besides the ones serving requests
# therefore reports their cost as part of a request's, and a comparison
# against a single-worker nginx then charges us for work nginx has no
# equivalent of. Before concluding that a request is expensive, this says
# which thread the time belongs to.
#
# Ticks, not percentages: a thread that costs nothing shows zero.
set -uo pipefail

. /bench/use_mounted.sh
lbbench_use_mounted threads.sh "$@"

DURATION="${DURATION:-15}"
CONNECTIONS="${CONNECTIONS:-50}"
THREADS="${THREADS:-2}"
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

CLK=$(getconf CLK_TCK 2>/dev/null || echo 100)

nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
    curl -sf -o /dev/null "http://127.0.0.1:$p/" || die "backend on $p never answered"
done

say "building aether ..."
cp -r /src /build && cd /build && rm -rf build
make -j"$(nproc)" >/tmp/build.log 2>&1 || { tail -20 /tmp/build.log >&2; die "build failed"; }
./build/ae build benchmarks/http/lb_reuse_lb.ae -o /tmp/ae-lb >/tmp/lbbuild.log 2>&1 \
    || { tail -20 /tmp/lbbuild.log >&2; die "lb build failed"; }

BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
    taskset -c "$LB_CPUS" /tmp/ae-lb >/tmp/ae-lb.log 2>&1 &
PID=$!
for _ in $(seq 1 40); do
    body=$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null) && case "$body" in backend-*) break ;; esac
    sleep 0.25
done
case "$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null)" in
    backend-*) ;; *) die "balancer is not proxying" ;;
esac

# utime+stime per thread. Fields 14 and 15 counted from after the
# parenthesised comm, so a thread name with a space does not shift them.
snap() {
    local t tid line rest
    for t in /proc/"$PID"/task/*; do
        tid=${t##*/}
        line=$(cat "$t/stat" 2>/dev/null) || continue
        rest=${line#*") "}
        printf '%s %s %s\n' "$tid" \
            "$(printf '%s' "$rest" | awk '{print $12 + $13}')" \
            "$(tr -d '\n' < "$t/comm" 2>/dev/null)"
    done
}

taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d3s \
    http://127.0.0.1:18200/ >/dev/null 2>&1

snap > /tmp/threads-before.txt
taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" \
    http://127.0.0.1:18200/ >/tmp/wrk.out 2>&1
snap > /tmp/threads-after.txt

REQS=$(awk '/requests in/ {print $1; exit}' /tmp/wrk.out)
[ -n "$REQS" ] || die "wrk reported no requests"

printf '\n%s\n' "== CPU by thread over $REQS requests ==" >&2
awk -v reqs="$REQS" -v clk="$CLK" '
    NR == FNR { before[$1] = $2; next }
    {
        d = $2 - before[$1]
        if (d > 0) { rows[++n] = sprintf("  %-8s %-18s %8.2f us/req", $1, $3, d * 1000000.0 / clk / reqs) }
        tot += d
    }
    END {
        for (i = 1; i <= n; i++) print rows[i] > "/dev/stderr"
        printf "  %-8s %-18s %8.2f us/req\n", "ALL", "", tot * 1000000.0 / clk / reqs > "/dev/stderr"
    }' /tmp/threads-before.txt /tmp/threads-after.txt

kill "$PID" 2>/dev/null
say ""
say "A thread with no equivalent in the subject being compared against is the"
say "first thing to explain before treating the difference as request cost."
