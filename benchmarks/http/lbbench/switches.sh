#!/bin/bash
# Which calls put a worker to sleep, by name?
#
# run.sh counts context switches and splits voluntary from involuntary, which
# says how many and of what kind but not where. This records the scheduler
# tracepoint with stacks, so each sleep is attributed to the call that asked
# for it. #1758 turns on removing two voluntary sleeps per request, and this
# is what says which two they are rather than which two look likely.
#
# Voluntary only: a task leaving the CPU in state S (interruptible sleep) went
# to sleep because the code asked. A task leaving in state R was preempted and
# says nothing about the code.
set -uo pipefail

. /bench/use_mounted.sh
lbbench_use_mounted switches.sh "$@"

DURATION="${DURATION:-10}"
CONNECTIONS="${CONNECTIONS:-50}"
THREADS="${THREADS:-2}"
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

PERF=$(ls /usr/lib/linux-tools-*/perf 2>/dev/null | head -1)
[ -n "$PERF" ] || PERF=$(command -v perf)
[ -n "$PERF" ] || die "perf not installed in the image"

sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1 \
    && say "perf_event_paranoid set to -1" \
    || say "could not lower perf_event_paranoid (now $(cat /proc/sys/kernel/perf_event_paranoid)); tracepoints may be unavailable"

"$PERF" list sched:sched_switch 2>/dev/null | grep -q sched_switch \
    || die "sched:sched_switch tracepoint unavailable; needs a privileged container"

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

taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d5s http://127.0.0.1:18200/ >/dev/null 2>&1

say "recording sleeps for ${DURATION}s under load ..."
taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" \
    http://127.0.0.1:18200/ >/tmp/wrk.out 2>&1 &
WRK=$!
"$PERF" record -e sched:sched_switch -g --pid "$LB" -o /tmp/sw.data \
    -- sleep "$DURATION" >/dev/null 2>&1
wait "$WRK" 2>/dev/null
REQS=$(awk '/requests in/ {print $1; exit}' /tmp/wrk.out)
kill "$LB" 2>/dev/null

[ -s /tmp/sw.data ] || die "no samples recorded"
say ""
say "requests during the recording: ${REQS:-?}"

# One line per sample: the previous task's state, then its stack, innermost
# first. prev_state S is a voluntary sleep; R is preemption.
"$PERF" script -i /tmp/sw.data --no-inline 2>/dev/null > /tmp/sw.txt

awk -v reqs="${REQS:-0}" '
    # perf script prints one record per switch: a header line carrying
    # prev_state, then the stack, innermost frame first, each frame as
    # "<addr> <symbol>+<off> (<module>)".
    /prev_state=/ {
        state = "?"
        if (match($0, /prev_state=[A-Z]+/))
            state = substr($0, RSTART + 11, RLENGTH - 11)
        collecting = (state == "S")
        wait = ""; ours = ""; next
    }
    collecting && /^\t/ {
        sym = $2
        sub(/\+0x[0-9a-f]+$/, "", sym)
        mod = $NF
        # The kernel function that asked to sleep, skipping the scheduler
        # plumbing every sleep goes through.
        if (wait == "" && sym !~ /^(schedule|__schedule|schedule_timeout|io_schedule)/ \
            && sym !~ /^(0x|\[unknown\])/ && sym != "")
            wait = sym
        # The innermost frame that is our own code, which is the call site
        # worth changing. Kernel frames name the mechanism, not the caller.
        if (ours == "" && mod ~ /ae-lb/ && sym !~ /^(0x|\[unknown\])/ && sym != "")
            ours = sym
        next
    }
    /^$/ {
        if (collecting && wait != "") {
            site[wait "\t" (ours == "" ? "(no frame in ae-lb)" : ours)]++
            total++
        }
        collecting = 0
    }
    END {
        if (total == 0) { print "  no voluntary sleeps recorded"; exit }
        printf "\n== voluntary sleeps, by kernel wait site and our call, top 15 ==\n"
        printf "  %-24s %-28s %8s %8s\n", "sleeps in", "asked for by", "count", "per req"
        n = 0
        for (s in site) { k[n] = s; n++ }
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++)
                if (site[k[j]] > site[k[i]]) { t = k[i]; k[i] = k[j]; k[j] = t }
        for (i = 0; i < n && i < 15; i++) {
            split(k[i], f, "\t")
            printf "  %-24s %-28s %8d %8.2f\n", f[1], f[2], site[k[i]], \
                   (reqs > 0 ? site[k[i]] / reqs : 0)
        }
        printf "  %-24s %-28s %8d %8.2f\n", "TOTAL", "", total, (reqs > 0 ? total / reqs : 0)
    }' /tmp/sw.txt >&2
