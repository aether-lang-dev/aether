#!/bin/bash
# Measure aether's load balancer against nginx and haproxy, same box, same run.
#
# The point is the ratio, not the absolute numbers: all three proxy to the same
# two backends, through the same generator, in one session, so box speed and
# container overhead cancel out. A number from this harness is only comparable
# to another number from this harness.
#
# The aether tree is mounted at /src and built here.
set -uo pipefail

DURATION="${DURATION:-20s}"
CONNECTIONS="${CONNECTIONS:-50}"
THREADS="${THREADS:-2}"
WARMUP="${WARMUP:-5s}"
# Pin the generator and the balancer to different cores so they do not fight.
# Everything below assumes at least 4 usable CPUs; with fewer, the generator
# and the subject share and the numbers say more about scheduling than proxying.
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

. /bench/use_mounted.sh
lbbench_use_mounted run.sh "$@"

[ "$(nproc)" -ge 4 ] || die "need >= 4 CPUs, have $(nproc). Give the container more."

# ---- backends (identical for every balancer) --------------------------------
nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
    curl -sf -o /dev/null "http://127.0.0.1:$p/" || die "backend on $p never answered"
done
say "backends up on 19001 and 19002"

# ---- build the aether load balancer(s) --------------------------------------
# AB_REF builds a second balancer from that git ref of the same tree and
# alternates the two in every round. Comparing across separate runs does not
# work: a run of this harness measured nginx at 56,572 rps and the next at
# 40,058 on the same box and the same config, so an aether delta taken between
# them would have been reporting the weather. Alternating inside one session,
# with the controls in the same session, is what makes a difference readable.
build_lb() {                      # build_lb <srcdir> <out>
    local src="$1" out="$2"
    ( cd "$src" && rm -rf build \
      && make -j"$(nproc)" >/tmp/build-$(basename "$out").log 2>&1 \
      && ./build/ae build benchmarks/http/lb_reuse_lb.ae -o "$out" \
           >>/tmp/build-$(basename "$out").log 2>&1 ) \
      || { tail -25 /tmp/build-$(basename "$out").log >&2; die "build failed for $src"; }
}

say "building aether (this is the slow part) ..."
cp -r /src /build
build_lb /build /tmp/ae-lb
say "aether $(cat /build/VERSION) built"

AB_REF="${AB_REF:-}"
if [ -n "$AB_REF" ]; then
    say "building the comparison at $AB_REF ..."
    git clone -q /src /baseline 2>/dev/null || die "cannot clone /src (mount it as a git repo)"
    ( cd /baseline && git checkout -q "$AB_REF" ) || die "no such ref: $AB_REF"
    build_lb /baseline /tmp/ae-lb-base
    say "comparison at $AB_REF built"
fi

stop_lb() {
    pkill -f '/tmp/ae-lb'   >/dev/null 2>&1
    nginx -c /bench/nginx-lb.conf -p /tmp -s quit >/dev/null 2>&1
    pkill -x haproxy        >/dev/null 2>&1
    sleep 1
    rm -f /tmp/lb.pid
}

# nginx daemonizes, so the pid of the process the shell started is not the
# master's. The master's is in the pid file and is not written yet when the
# launcher returns, so it has to be waited for; a stale pid from the previous
# round names a dead process and every figure derived from it reads 0.
wait_pid_file() {                 # wait_pid_file <file>
    local f="$1" i pid
    for i in $(seq 1 60); do
        pid=$(cat "$f" 2>/dev/null || true)
        if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then echo "$pid"; return 0; fi
        sleep 0.1
    done
    return 1
}

# Refuse to report a number for a balancer that is not actually proxying. A
# balancer answering errors quickly reads as fast: #1720 saw a +26% that was
# entirely dead backends.
verify() {
    local body
    body=$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null) || return 1
    case "$body" in backend-*) return 0 ;; *) return 1 ;; esac
}

# Context switches the balancer's threads made, from /proc. A thread-per-request
# server pays these where an event loop does not, so this is the number that
# prices the thread model, and it is what a move to non-blocking upstream I/O
# would have to reduce to be worth doing.
# Every pid in the balancer's process tree. nginx forks workers and does its
# work there, so following the master alone reported 0 CPU and 0 context
# switches for it, a flattering number that says nothing.
lb_pids() {                       # lb_pids <pid>
    local pid="$1" kids
    [ -d "/proc/$pid" ] || return
    echo "$pid"
    kids=$(pgrep -P "$pid" 2>/dev/null)
    for k in $kids; do lb_pids "$k"; done
}

# Voluntary and involuntary switches are counted separately. Voluntary means
# the code asked to sleep; involuntary means the scheduler took the CPU away,
# which is threads oversubscribing the cores. A summed figure cannot tell them
# apart and they call for opposite fixes.
lb_ctxt() {                       # lb_ctxt <pid> <voluntary|involuntary>
    local pid="$1" which="$2" total=0 t v p key
    case "$which" in
        voluntary)   key='^voluntary_ctxt_switches' ;;
        involuntary) key='^nonvoluntary_ctxt_switches' ;;
        *) echo 0; return ;;
    esac
    [ -d "/proc/$pid" ] || { echo 0; return; }
    for p in $(lb_pids "$pid"); do
    for t in /proc/"$p"/task/*/status; do
        [ -r "$t" ] || continue
        v=$(awk -v k="$key" '$0 ~ k {s += $2} END {print s+0}' "$t")
        total=$((total + v))
    done
    done
    echo "$total"
}

# CPU the balancer itself burned, in jiffies, from its own /proc entry. Summed
# over the process and its threads.
# Minor page faults the balancer's process tree took, from /proc. A fault is
# the kernel handing over a fresh page, which it then has to zero, and page
# clearing has been the largest single entry in this path's profile. Unlike
# CPU per request it is a count, so a busy box does not move it: it measures
# how much memory the code churns, which is what an allocation change is for.
lb_faults() {                     # lb_faults <pid>
    local pid="$1" total=0 p v
    [ -d "/proc/$pid" ] || { echo 0; return; }
    for p in $(lb_pids "$pid"); do
        [ -r "/proc/$p/stat" ] || continue
        v=$(awk '{print $10}' "/proc/$p/stat" 2>/dev/null)
        total=$((total + ${v:-0}))
    done
    echo "$total"
}

lb_cpu() {                        # lb_cpu <pid>
    local pid="$1" total=0 t u s2 p
    [ -d "/proc/$pid" ] || { echo 0; return; }
    for p in $(lb_pids "$pid"); do
    for t in /proc/"$p"/task/*/stat; do
        [ -r "$t" ] || continue
        u=$(awk '{print $14}' "$t"); s2=$(awk '{print $15}' "$t")
        total=$((total + u + s2))
    done
    done
    echo "$total"
}

measure() {                       # measure <label> [pid]
    local label="$1" pid="${2:-}" out rps p99 errs cpu0 cpu1 reqs cpu_us
    for _ in $(seq 1 40); do verify && break; sleep 0.25; done
    verify || { say "  $label: NOT PROXYING, skipped"; return; }
    taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"$WARMUP" \
        http://127.0.0.1:18200/ >/dev/null 2>&1

    cpu0=$(lb_cpu "$pid"); vol0=$(lb_ctxt "$pid" voluntary)
    inv0=$(lb_ctxt "$pid" involuntary); flt0=$(lb_faults "$pid")
    out=$(taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" \
        --latency http://127.0.0.1:18200/ 2>&1)
    cpu1=$(lb_cpu "$pid"); vol1=$(lb_ctxt "$pid" voluntary)
    inv1=$(lb_ctxt "$pid" involuntary); flt1=$(lb_faults "$pid")

    rps=$(printf '%s' "$out" | awk '/^Requests\/sec:/ {print $2}')
    p99=$(printf '%s' "$out" | awk '/99%/ {print $2; exit}')
    errs=$(printf '%s' "$out" | awk '/Socket errors/ {print; exit}')
    reqs=$(printf '%s' "$out" | awk '/requests in/ {print $1; exit}')

    # CPU microseconds the balancer spent per request it served. Far steadier
    # than rps on a shared box: rps depends on everything else running, this
    # depends on the work the code does. A jiffy is 10ms at the usual 100Hz.
    # A CPU figure this could not measure is reported as unmeasured, never as
    # zero. Zero reads as "burned no CPU", which is never true of a process
    # that served requests, and it silently hides the subject's real cost.
    cpu_us=""; ctx_per=""; vol_per=""; inv_per=""; flt_per=""; note=""
    if [ -z "$pid" ] || [ ! -d "/proc/$pid" ]; then
        note="cpu UNMEASURED (no live pid)  "
    elif [ "${reqs:-0}" -gt 0 ] 2>/dev/null; then
        if [ "$((cpu1 - cpu0))" -le 0 ]; then
            note="cpu UNMEASURED (pid $pid burned none: wrong process?)  "
        else
            cpu_us=$(awk -v d="$((cpu1 - cpu0))" -v r="$reqs" \
                         'BEGIN { if (r > 0) printf "%.1f", d * 10000 / r }')
            vol_per=$(awk -v d="$((vol1 - vol0))" -v r="$reqs" \
                         'BEGIN { if (r > 0) printf "%.2f", d / r }')
            inv_per=$(awk -v d="$((inv1 - inv0))" -v r="$reqs" \
                         'BEGIN { if (r > 0) printf "%.2f", d / r }')
            ctx_per=$(awk -v a="$((vol1 - vol0))" -v b="$((inv1 - inv0))" \
                         -v r="$reqs" 'BEGIN { if (r > 0) printf "%.2f", (a+b)/r }')
            flt_per=$(awk -v d="$((flt1 - flt0))" -v r="$reqs" \
                         'BEGIN { if (r > 0) printf "%.2f", d / r }')
        fi
    fi
    # stderr, like every other line this prints. A result on stdout and the
    # round header on stderr interleave once the output is piped, which files
    # a row under the wrong round.
    printf '%-10s %12s rps   p99 %-9s %s%s%s%s%s\n' "$label" "${rps:-?}" "${p99:-?}" \
        "$note" "${cpu_us:+cpu ${cpu_us}us/req  }" \
        "${ctx_per:+ctxsw ${ctx_per}/req (vol ${vol_per} inv ${inv_per})  }" \
        "${flt_per:+faults ${flt_per}/req  }" "${errs:-}" >&2
    echo "$label $rps ${cpu_us:-NA} ${ctx_per:-NA} ${flt_per:-0}" >> /tmp/results.txt
}

: > /tmp/results.txt
printf '\n%s\n' "== $DURATION, $CONNECTIONS connections, generator on CPU $GEN_CPUS, balancer on CPU $LB_CPUS ==" >&2

run_aether() {                    # run_aether <binary> <label>
    stop_lb
    BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
        taskset -c "$LB_CPUS" "$1" >/tmp/ae-lb.log 2>&1 &
    measure "$2" "$!"
}

# Rounds, so a slow patch of the box shows up as spread rather than as a
# result. Every subject is measured once per round.
run_nginx()   { stop_lb; taskset -c "$LB_CPUS" nginx -c /bench/nginx-lb.conf -p /tmp
                measure nginx "$(wait_pid_file /tmp/lb.pid || true)"; }
run_haproxy() { stop_lb; taskset -c "$LB_CPUS" haproxy -f /bench/haproxy-lb.cfg
                measure haproxy "$(pgrep -x haproxy | head -1)"; }

# The order is reversed on even rounds. Measuring A before B every time gives
# any drift inside a round (the box warming, the page cache filling) to
# whichever runs second, every time, and that is indistinguishable from B
# being slower. Alternating cancels it instead of hoping it is absent.
ROUNDS="${ROUNDS:-3}"
for round in $(seq 1 "$ROUNDS"); do
    say "-- round $round of $ROUNDS"
    if [ $((round % 2)) -eq 1 ]; then
        [ -n "$AB_REF" ] && run_aether /tmp/ae-lb-base baseline
        run_aether /tmp/ae-lb aether
        run_nginx
        run_haproxy
    else
        run_haproxy
        run_nginx
        run_aether /tmp/ae-lb aether
        [ -n "$AB_REF" ] && run_aether /tmp/ae-lb-base baseline
    fi
done
stop_lb

say ""
# Median per subject, and the spread, because a median without a spread hides
# exactly the noise that makes a comparison worthless.
#
# Written for mawk (Ubuntu's default awk): no arrays-of-arrays, which is a
# gawk extension. Values are keyed "subject SUBSEP index" instead.
awk '
    { n[$1]++; v[$1 SUBSEP n[$1]] = $2 + 0
      if ($3 == "NA") { na[$1] = 1 } else { cn[$1]++; cpu[$1 SUBSEP cn[$1]] = $3 + 0; cx[$1 SUBSEP cn[$1]] = $4 + 0; fl[$1 SUBSEP cn[$1]] = $5 + 0 } }
    END {
        printf "%-10s %10s %10s %10s %6s\n", "subject", "median", "min", "max", "runs"
        for (s in n) {
            c = n[s]
            for (i = 1; i <= c; i++)
                for (j = i + 1; j <= c; j++)
                    if (v[s SUBSEP j] < v[s SUBSEP i]) {
                        t = v[s SUBSEP i]; v[s SUBSEP i] = v[s SUBSEP j]; v[s SUBSEP j] = t
                    }
            med = (c % 2) ? v[s SUBSEP int((c+1)/2)] \
                          : (v[s SUBSEP c/2] + v[s SUBSEP c/2+1]) / 2
            # median cpu/req over the rounds that could be measured, sorted
            # independently of the rps ordering
            cc = cn[s]
            for (i = 1; i <= cc; i++) cs[i] = cpu[s SUBSEP i]
            for (i = 1; i <= cc; i++)
                for (j = i + 1; j <= cc; j++)
                    if (cs[j] < cs[i]) { t = cs[i]; cs[i] = cs[j]; cs[j] = t }
            for (i = 1; i <= cc; i++) xs[i] = cx[s SUBSEP i]
            for (i = 1; i <= cc; i++)
                for (j = i + 1; j <= cc; j++)
                    if (xs[j] < xs[i]) { t = xs[i]; xs[i] = xs[j]; xs[j] = t }
            printf "%-10s %10.0f %10.0f %10.0f %6d", s, med, v[s SUBSEP 1], v[s SUBSEP c], c
            if (cc > 0) {
                cmed = (cc % 2) ? cs[int((cc+1)/2)] : (cs[cc/2] + cs[cc/2+1]) / 2
                xmed = (cc % 2) ? xs[int((cc+1)/2)] : (xs[cc/2] + xs[cc/2+1]) / 2
                # The least-contended round is the closest this box gets to
                # the cost of the code alone. Contention only ever adds, so
                # the smallest CPU per request is the better estimator on a
                # busy machine and the median is the better one on a quiet
                # one. Both are printed; a large gap between them is itself
                # the warning.
                for (i = 1; i <= cc; i++) fs[i] = fl[s SUBSEP i]
                for (i = 1; i <= cc; i++)
                    for (j = i + 1; j <= cc; j++)
                        if (fs[j] < fs[i]) { t = fs[i]; fs[i] = fs[j]; fs[j] = t }
                fmed = (cc % 2) ? fs[int((cc+1)/2)] : (fs[cc/2] + fs[cc/2+1]) / 2
                printf "   %8.1f us/req (min %6.1f)  %7.2f ctxsw/req  %6.2f faults/req", cmed, cs[1], xmed, fmed
                if (na[s]) printf "  (%d round(s) unmeasured)", n[s] - cc
                mc[s] = cmed; mcmin[s] = cs[1]
            } else printf "   %19s", "cpu UNMEASURED"
            printf "\n"
            m[s] = med; lo[s] = v[s SUBSEP 1]; hi[s] = v[s SUBSEP c]
        }
        if (m["nginx"] > 0 && m["aether"] > 0)
            printf "\naether is %.1f%% of nginx\n", 100 * m["aether"] / m["nginx"]
        if (m["baseline"] > 0 && m["aether"] > 0) {
            printf "aether vs baseline, rps:     %+.1f%%\n", 100 * (m["aether"] - m["baseline"]) / m["baseline"]
            if (mc["baseline"] > 0 && mc["aether"] > 0)
                printf "aether vs baseline, cpu/req: %+.1f%% by median, %+.1f%% by least-contended round\n", \
                    100 * (mc["aether"] - mc["baseline"]) / mc["baseline"], \
                    100 * (mcmin["aether"] - mcmin["baseline"]) / mcmin["baseline"]
        }
        if (m["nginx"] > 0) {
            spread = 100 * (hi["nginx"] - lo["nginx"]) / m["nginx"]
            printf "nginx spread across rounds: %.1f%%", spread
            if (spread > 10)
                printf "  <-- controls moved this much; treat any delta above as unreadable"
            printf "\n"
        }
    }' /tmp/results.txt >&2
