#!/bin/bash
# How much work does the balancer do per request, in instructions?
#
# run.sh measures CPU microseconds, which is the right thing to judge a result
# but moves with everything else on the box: on a loaded machine its medians
# have disagreed with each other by 20% between two builds that turned out to
# differ by 1%. An instruction count is very nearly independent of what else is
# running, so it answers a narrower question exactly: did this change make the
# code do more work per request?
#
# It cannot replace run.sh. Instructions say nothing about stalls, cache
# misses, or time spent asleep, which is where most of this path's cost is. Use
# it to price a change in userspace work, and run.sh to judge the result.
set -uo pipefail

. /bench/use_mounted.sh
lbbench_use_mounted instructions.sh "$@"

DURATION="${DURATION:-10}"
CONNECTIONS="${CONNECTIONS:-8}"
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

PERF=$(ls /usr/lib/linux-tools-*/perf 2>/dev/null | head -1)
[ -n "$PERF" ] || PERF=$(command -v perf)
[ -n "$PERF" ] || die "perf not installed in the image"

sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1 || true

# perf exits 0 whether or not the counter exists, printing "<not supported>"
# where the number would be. A virtual machine usually has no performance
# monitoring unit to expose, which is the common case for this image, so the
# probe has to read the output rather than trust the exit status.
probe=$("$PERF" stat -e instructions -x, true 2>&1)
case "$probe" in
    *not\ supported*|*not\ counted*|*"<"*)
        die "this machine does not expose an instruction counter (no PMU, which is usual inside a VM); use run.sh and profile.sh here" ;;
esac
case "$probe" in
    [0-9]*) ;;
    *) die "could not read an instruction count from perf: $probe" ;;
esac

nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
done

build_lb() {                      # build_lb <srcdir> <out>
    ( cd "$1" && rm -rf build && make -j"$(nproc)" >/tmp/b.log 2>&1 \
      && ./build/ae build benchmarks/http/lb_reuse_lb.ae -o "$2" >>/tmp/b.log 2>&1 ) \
      || { tail -20 /tmp/b.log >&2; die "build failed for $1"; }
}

say "building ..."
cp -r /src /build && build_lb /build /tmp/ae-lb
AB_REF="${AB_REF:-}"
if [ -n "$AB_REF" ]; then
    git clone -q /src /baseline || die "cannot clone /src"
    ( cd /baseline && git checkout -q "$AB_REF" ) || die "no such ref: $AB_REF"
    build_lb /baseline /tmp/ae-lb-base
fi

measure_for() {                   # measure_for <binary> <label>
    local bin="$1" label="$2" out reqs insns
    pkill -f '/tmp/ae-lb' >/dev/null 2>&1; sleep 1
    BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
        taskset -c "$LB_CPUS" "$bin" >/tmp/lb-"$label".log 2>&1 &
    local pid=$!
    for _ in $(seq 1 60); do
        out=$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null) && case "$out" in backend-*) break ;; esac
        sleep 0.5
    done
    case "$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null)" in
        backend-*) ;; *) die "$label is not proxying" ;;
    esac

    # Warm first, so start-up work is not counted against the requests below.
    taskset -c "$GEN_CPUS" wrk -t2 -c"$CONNECTIONS" -d3s http://127.0.0.1:18200/ >/dev/null 2>&1

    # Count only while the load runs, and count the whole process tree.
    taskset -c "$GEN_CPUS" wrk -t2 -c"$CONNECTIONS" -d"${DURATION}s" \
        http://127.0.0.1:18200/ >/tmp/wrk-"$label".out 2>&1 &
    local wrk_pid=$!
    "$PERF" stat -e instructions -p "$pid" -o /tmp/perf-"$label".txt \
        -- sleep "$DURATION" >/dev/null 2>&1
    wait "$wrk_pid" 2>/dev/null

    reqs=$(awk '/requests in/ {print $1; exit}' /tmp/wrk-"$label".out)
    insns=$(awk '/instructions/ && $1 ~ /^[0-9,]+$/ {gsub(/,/, "", $1); print $1; exit}' \
                /tmp/perf-"$label".txt)
    kill "$pid" 2>/dev/null

    # A count that could not be read is reported as unmeasured, never as 0:
    # zero instructions per request is not a fast balancer, it is a missing
    # counter, and printing it as a number invites a comparison against one.
    if [ -z "${reqs:-}" ] || [ -z "${insns:-}" ] \
       || [ "${reqs:-0}" -le 0 ] 2>/dev/null || [ "${insns:-0}" -le 0 ] 2>/dev/null; then
        say "  $label: UNMEASURED (requests=${reqs:-?}, instruction count=${insns:-none})"
        say "           perf said: $(grep -m1 instructions /tmp/perf-"$label".txt | sed 's/^ *//')"
        return
    fi
    awk -v l="$label" -v i="$insns" -v r="$reqs" \
        'BEGIN { printf "  %-10s %14.0f instructions/req  (%s requests)\n", l, i / r, r }' >&2
}

say ""
say "== instructions per proxied request =="
measure_for /tmp/ae-lb current
[ -n "$AB_REF" ] && measure_for /tmp/ae-lb-base baseline
pkill -f '/tmp/ae-lb' >/dev/null 2>&1
