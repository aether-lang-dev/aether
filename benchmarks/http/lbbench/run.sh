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
}

# Refuse to report a number for a balancer that is not actually proxying. A
# balancer answering errors quickly reads as fast: #1720 saw a +26% that was
# entirely dead backends.
verify() {
    local body
    body=$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null) || return 1
    case "$body" in backend-*) return 0 ;; *) return 1 ;; esac
}

# CPU the balancer itself burned, in jiffies, from its own /proc entry. Summed
# over the process and its threads.
lb_cpu() {                        # lb_cpu <pid>
    local pid="$1" total=0 t u s2
    [ -d "/proc/$pid" ] || { echo 0; return; }
    for t in /proc/"$pid"/task/*/stat; do
        [ -r "$t" ] || continue
        u=$(awk '{print $14}' "$t"); s2=$(awk '{print $15}' "$t")
        total=$((total + u + s2))
    done
    echo "$total"
}

measure() {                       # measure <label> [pid]
    local label="$1" pid="${2:-}" out rps p99 errs cpu0 cpu1 reqs cpu_us
    for _ in $(seq 1 40); do verify && break; sleep 0.25; done
    verify || { say "  $label: NOT PROXYING — skipped"; echo "$label FAILED 0"; return; }
    taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"$WARMUP" \
        http://127.0.0.1:18200/ >/dev/null 2>&1

    cpu0=$(lb_cpu "$pid")
    out=$(taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" \
        --latency http://127.0.0.1:18200/ 2>&1)
    cpu1=$(lb_cpu "$pid")

    rps=$(printf '%s' "$out" | awk '/^Requests\/sec:/ {print $2}')
    p99=$(printf '%s' "$out" | awk '/99%/ {print $2; exit}')
    errs=$(printf '%s' "$out" | awk '/Socket errors/ {print; exit}')
    reqs=$(printf '%s' "$out" | awk '/requests in/ {print $1; exit}')

    # CPU microseconds the balancer spent per request it served. Far steadier
    # than rps on a shared box: rps depends on everything else running, this
    # depends on the work the code does. A jiffy is 10ms at the usual 100Hz.
    cpu_us=""
    if [ -n "$pid" ] && [ "${reqs:-0}" -gt 0 ] 2>/dev/null; then
        cpu_us=$(awk -v d="$((cpu1 - cpu0))" -v r="$reqs" \
                     'BEGIN { if (r > 0) printf "%.1f", d * 10000 / r }')
    fi
    printf '%-10s %12s rps   p99 %-9s %s%s\n' "$label" "${rps:-?}" "${p99:-?}" \
        "${cpu_us:+cpu ${cpu_us}us/req  }" "${errs:-}"
    echo "$label $rps ${cpu_us:-0}" >> /tmp/results.txt
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
# result. Every subject is measured once per round, in the same order.
run_nginx()   { stop_lb; taskset -c "$LB_CPUS" nginx -c /bench/nginx-lb.conf -p /tmp
                measure nginx "$(cat /tmp/lb.pid 2>/dev/null)"; }
run_haproxy() { stop_lb; taskset -c "$LB_CPUS" haproxy -f /bench/haproxy-lb.cfg
                measure haproxy "$(pgrep -x haproxy | head -1)"; }

# The order is reversed on even rounds. Measuring A before B every time gives
# any drift inside a round — the box warming, the page cache filling — to
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
    { n[$1]++; v[$1 SUBSEP n[$1]] = $2 + 0; cpu[$1 SUBSEP n[$1]] = $3 + 0 }
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
            # median cpu/req, sorted independently of the rps ordering
            for (i = 1; i <= c; i++) cs[i] = cpu[s SUBSEP i]
            for (i = 1; i <= c; i++)
                for (j = i + 1; j <= c; j++)
                    if (cs[j] < cs[i]) { t = cs[i]; cs[i] = cs[j]; cs[j] = t }
            cmed = (c % 2) ? cs[int((c+1)/2)] : (cs[c/2] + cs[c/2+1]) / 2
            printf "%-10s %10.0f %10.0f %10.0f %6d   %8.1f us/req\n", s, med, v[s SUBSEP 1], v[s SUBSEP c], c, cmed
            m[s] = med; lo[s] = v[s SUBSEP 1]; hi[s] = v[s SUBSEP c]; mc[s] = cmed
        }
        if (m["nginx"] > 0 && m["aether"] > 0)
            printf "\naether is %.1f%% of nginx\n", 100 * m["aether"] / m["nginx"]
        if (m["baseline"] > 0 && m["aether"] > 0) {
            printf "aether vs baseline, rps:     %+.1f%%\n", 100 * (m["aether"] - m["baseline"]) / m["baseline"]
            if (mc["baseline"] > 0 && mc["aether"] > 0)
                printf "aether vs baseline, cpu/req: %+.1f%%  (lower is better, and steadier than rps here)\n", \
                    100 * (mc["aether"] - mc["baseline"]) / mc["baseline"]
        }
        if (m["nginx"] > 0) {
            spread = 100 * (hi["nginx"] - lo["nginx"]) / m["nginx"]
            printf "nginx spread across rounds: %.1f%%", spread
            if (spread > 10)
                printf "  <-- controls moved this much; treat any delta above as unreadable"
            printf "\n"
        }
    }' /tmp/results.txt >&2
