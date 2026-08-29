#!/bin/bash
# Where a proxied request's CPU goes: user space or the kernel, for every
# subject, under the same load.
#
# run.sh says how much CPU a request costs and syscalls.sh says how many calls
# it makes. When those two disagree -- the same syscall count as nginx, and
# twice the CPU -- neither says which half the extra is in, and the answer
# decides what to work on. Time in the kernel at parity syscall counts means
# the calls are carrying more data or touching more pages; time in user space
# means the work before the call is the cost.
#
# The split comes from utime and stime in /proc, not from a hardware counter:
# cycles and instructions read <not supported> on a virtual machine that does
# not expose a PMU, which is most of them, and this measurement is too useful
# to be available only on bare metal. Time is summed across every process of
# the subject, so a master and its workers are counted together.
#
# The split is what to read. The absolute microseconds still move with load.
set -uo pipefail

. /bench/use_mounted.sh
lbbench_use_mounted split.sh "$@"

DURATION="${DURATION:-15}"
CONNECTIONS="${CONNECTIONS:-50}"
THREADS="${THREADS:-2}"
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

CLK=$(getconf CLK_TCK 2>/dev/null || echo 100)

# utime and stime for a subject, in clock ticks: the process named plus its
# direct children. Fields 14 and 15 of /proc/<pid>/stat, counted from after
# the parenthesised comm so a name containing a space or a bracket does not
# shift them. A process's own threads are already included in its totals.
#
# The tree, rather than a name match: the backends in this harness are nginx
# too, so matching on "nginx" would add their work to the balancer's and
# report a control that looks far more expensive than it is. nginx's workers
# are children of the master, which is what makes the tree the right set.
cpu_ticks() {                     # cpu_ticks <root-pid>
    local root="$1" total_u=0 total_s=0 pid line rest u s
    for pid in $root $(pgrep -P "$root" 2>/dev/null); do
        [ -r "/proc/$pid/stat" ] || continue
        line=$(cat "/proc/$pid/stat" 2>/dev/null) || continue
        rest=${line#*") "}
        u=$(printf '%s' "$rest" | awk '{print $12}')
        s=$(printf '%s' "$rest" | awk '{print $13}')
        total_u=$((total_u + ${u:-0}))
        total_s=$((total_s + ${s:-0}))
    done
    printf '%s %s' "$total_u" "$total_s"
}

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

stop_lb() {
    pkill -f '/tmp/ae-lb' >/dev/null 2>&1
    nginx -c /bench/nginx-lb.conf -p /tmp -s quit >/dev/null 2>&1
    pkill -x haproxy >/dev/null 2>&1
    sleep 1
    rm -f /tmp/lb.pid
}

wait_pid_file() {
    local f="$1" i pid
    for i in $(seq 1 60); do
        pid=$(cat "$f" 2>/dev/null || true)
        if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then echo "$pid"; return 0; fi
        sleep 0.1
    done
    return 1
}

# A balancer answering errors quickly reads as cheap, so nothing is reported
# for one that is not actually proxying.
verify() {
    local body
    body=$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null) || return 1
    case "$body" in backend-*) return 0 ;; *) return 1 ;; esac
}

measure() {                       # measure <label> <pid>
    local label="$1" pid="$2"
    [ -n "$pid" ] && [ -d "/proc/$pid" ] || { say "  $label: no pid, skipped"; return; }
    for _ in $(seq 1 40); do verify && break; sleep 0.25; done
    verify || { say "  $label: not proxying, skipped"; return; }

    taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d3s \
        http://127.0.0.1:18200/ >/dev/null 2>&1

    local before after bu bs au as
    before=$(cpu_ticks "$pid"); bu=${before% *}; bs=${before#* }

    taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" \
        http://127.0.0.1:18200/ >/tmp/wrk.out 2>&1

    after=$(cpu_ticks "$pid"); au=${after% *}; as=${after#* }

    local reqs
    reqs=$(awk '/requests in/ {print $1}' /tmp/wrk.out)
    [ -n "$reqs" ] || { say "  $label: wrk reported no requests, skipped"; return; }

    awk -v l="$label" -v r="$reqs" -v du=$((au - bu)) -v ds=$((as - bs)) -v clk="$CLK" 'BEGIN {
        if (r+0 == 0)            { printf "  %-9s no requests\n", l > "/dev/stderr"; exit }
        if (du+ds <= 0)          { printf "  %-9s UNMEASURED (no CPU time seen; wrong process matched?)\n", l > "/dev/stderr"; exit }
        us_user = du * 1000000.0 / clk / r
        us_krnl = ds * 1000000.0 / clk / r
        tot = us_user + us_krnl
        printf "  %-9s %6.1f us/req   user %5.1f (%4.1f%%)   kernel %5.1f (%4.1f%%)\n",
               l, tot, us_user, 100*us_user/tot, us_krnl, 100*us_krnl/tot > "/dev/stderr"
    }'
}

printf '\n%s\n' "== CPU per proxied request, user vs kernel ==" >&2

stop_lb
BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
    taskset -c "$LB_CPUS" /tmp/ae-lb >/tmp/ae-lb.log 2>&1 &
measure aether "$!"

stop_lb
taskset -c "$LB_CPUS" nginx -c /bench/nginx-lb.conf -p /tmp
measure nginx "$(wait_pid_file /tmp/lb.pid || true)"

stop_lb
taskset -c "$LB_CPUS" haproxy -f /bench/haproxy-lb.cfg
measure haproxy "$(pgrep -x haproxy | head -1)"

stop_lb
say ""
say "Read the split, not the totals: the totals move with load, the split does not."
